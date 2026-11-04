#include "build_ir.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

#include "distorm.h"
#include "mnemonics.h"

namespace swift::x86 {
namespace {
using namespace runtime::ir;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

class Emitter;
template<OpCode op = OpCode::StoreMemoryTSO>
void VWrite(Emitter&, StateLayout, const _DInst&, const _Operand&, Value, u64, u64, TargetInfo, bool aligned = false);

class Emitter final : public Builder {
public:
    Emitter(Scope scope, TargetInfo target, StateLayout layout)
            : Builder(scope, target), layout(layout), private_gprs(target.Has(TargetFeature::PrivateGPRState)),
              private_vectors(target.Has(TargetFeature::PrivateVectorState)) {}
    Error BeginBlock(BlockRef block) {
        const auto error = Builder::BeginBlock(block);
        if (error == Error::None) {
            valid_gprs = 0; whole_gprs = 0; pending_gprs = 0; valid_vectors = 0; last_address = {};
        }
        return error;
    }
    Error EndBlock(Terminal terminal) {
        PublishRegisters();
        return Builder::EndBlock(terminal);
    }
    void PublishGPRs(std::uint16_t slots = UINT16_MAX);
    void PublishVectors();
    void PublishRegisters() { PublishGPRs(); PublishVectors(); }

private:
    Value AddressExpression(Type type, std::initializer_list<Operand> inputs);
    void MaterializeGPR(unsigned slot);
    struct CachedGPR { NodeId value, view; std::uint8_t view_first{}; Type field_type{Type::Void}; };
    static_assert(sizeof(CachedGPR) == 12);
    std::array<CachedGPR, 16> gprs;
    // Whole-parent coverage is independent of dirty: a U32 read lacks high32,
    // whereas an EAX write defines the full zero-high parent after publication.
    std::uint16_t valid_gprs{}, dirty_gprs{}, whole_gprs{}, pending_gprs{};
    NodeId last_address;
    std::array<NodeId, 16> vectors;
    std::uint16_t valid_vectors{}, dirty_vectors{};
    const StateLayout layout;
    const bool private_gprs, private_vectors;
    friend Value ReadRegister(Emitter&, StateLayout, unsigned, unsigned);
    friend void WriteRegister(Emitter&, StateLayout, const _Operand&, Value);
    friend Value Address(Emitter&, StateLayout, const _DInst&, const _Operand&, u64, TargetInfo, Value, u64*, Type);
    friend Value VRead(Emitter&, StateLayout, const _DInst&, const _Operand&, u64, u64, TargetInfo, bool);
    template<OpCode op>
    friend void VWrite(Emitter&, StateLayout, const _DInst&, const _Operand&, Value, u64, u64, TargetInfo, bool);
    friend bool Vector(Emitter&, StateLayout, const _DInst&, u64, u64, TargetInfo);
};

bool Vector(Emitter&, StateLayout, const _DInst&, u64 pc, u64 next_pc, TargetInfo);

struct Rejected { BuildError error; Error detail{Error::None}; };
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
                if (at == actual.end()) { same = false; break; }
                const auto cell = *at++;
                if (cell.Kind() != input.kind || cell.TypeView() != input.type ||
                    cell.Payload() != input.payload || cell.Count() != input.count || cell.Code() != input.code) {
                    same = false; break;
                }
            }
            if (same && at == actual.end()) return {UnitId(), last_address};
        }
    }
    const auto value = Require(Emit<OpCode::GetOperand>(type, inputs));
    last_address = cache ? value.node : NodeId{};
    return value;
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
    const auto combined = Require(Emit<OpCode::BitInsert>(type,
            {Operand::ValueOf({UnitId(), cached.value}), Operand::ValueOf({UnitId(), cached.view}),
             Operand::Immediate(cached.view_first, Type::U8), Operand::Immediate(TypeBits(field_type), Type::U8)}));
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
        Require(Emit<OpCode::StoreUniform>(Type::Void,
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
        Require(Emit<OpCode::StoreUniform>(Type::Void,
                {Operand::Uniform(layout.fprs_offset + slot * 16, Type::V128),
                 Operand::ValueOf({UnitId(), vectors[slot]}, Type::V128)}));
    }
}
[[noreturn]] void Unsupported() { throw Rejected{BuildError::Unsupported}; }

Value Constant(Emitter& builder, u64 bits, Type type = Type::U64) {
    return Require(builder.Emit<OpCode::LoadImm>(Type::Void, {Operand::Immediate(bits, type)}));
}
Value Add(Emitter& builder, Value left, Operand right) {
    return Require(builder.Emit<OpCode::Add>(Type::Void,
            {Operand::ValueOf(left), right}));
}
Type ScalarType(unsigned bits) {
    switch (bits) {
        case 8: return Type::U8;
        case 16: return Type::U16;
        case 32: return Type::U32;
        case 64: return Type::U64;
        default: Unsupported();
    }
}
u32 RegisterOffset(StateLayout layout, unsigned reg, unsigned bits, unsigned& slot) {
    unsigned byte{};
    if (bits == 8) {
        if (reg >= R_AL && reg <= R_BL) slot = reg - R_AL;
        else if (reg >= R_AH && reg <= R_BH) { slot = reg - R_AH; byte = 1; }
        else if (reg >= R_R8B && reg <= R_R15B) slot = 8 + reg - R_R8B;
        else if (reg >= R_SPL && reg <= R_DIL) slot = 4 + reg - R_SPL;
        else Unsupported();
    } else {
        const unsigned base = bits == 64 ? REGS64_BASE : bits == 32 ? REGS32_BASE : REGS16_BASE;
        if ((bits != 16 && bits != 32 && bits != 64) || reg < base || reg - base >= 16) Unsupported();
        slot = reg - base;
    }
    return layout.gprs_offset + slot * layout.stride + byte;
}
Value ReadRegister(Emitter& builder, StateLayout layout, unsigned reg, unsigned bits) {
    unsigned slot{};
    const auto offset = RegisterOffset(layout, reg, bits, slot);
    const auto type = ScalarType(bits);
    const bool cache = builder.private_gprs;
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    if (cache && (builder.valid_gprs & bit)) {
        auto& cached = builder.gprs[slot];
        const auto first = (offset - (layout.gprs_offset + slot * layout.stride)) * 8;
        if (builder.pending_gprs & bit) {
            if (cached.view_first == first && cached.field_type == type) {
                if (builder.Operations()[cached.view.value].type != type) {
                    const auto view = Require(builder.Emit<OpCode::BitExtract>(type,
                            {Operand::ValueOf({builder.UnitId(), cached.view}),
                             Operand::Immediate(0, Type::U8), Operand::Immediate(bits, Type::U8)}));
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
                const auto full = Require(builder.Emit<OpCode::ZeroExtend64>(Type::U64,
                        {Operand::ValueOf(value, Type::U32)}));
                cached.value = full.node;
                cached.view = node;
                cached.view_first = 0;
                return full;
            }
            // A mere EAX load did not capture RAX's unknown high half.
        } else {
            if (bits == 32 && natural == Type::U32) return value;
            if (cached.view && cached.view_first == first && builder.Operations()[cached.view.value].type == type)
                return {builder.UnitId(), cached.view};
            // A narrow read is a derived value, never the new parent cache.
            // Retain only the last view in this same fixed slot, avoiding
            // repeated normalizers without a value-indexed side table.
            const auto view = bits == 32
                    ? Require(builder.Emit<OpCode::ZeroExtend32>(Type::U32, {Operand::ValueOf(value, Type::U32)}))
                    : Require(builder.Emit<OpCode::BitExtract>(type,
                            {Operand::ValueOf(value), Operand::Immediate(first, Type::U8), Operand::Immediate(bits, Type::U8)}));
            cached.view = view.node;
            cached.view_first = static_cast<std::uint8_t>(first);
            return view;
        }
    }
    // Only an uncaptured parent range needs a real state read. Generic callers
    // keep their original typed Uniform accesses and observation behavior.
    builder.PublishGPRs(bit);
    const auto value = Require(builder.Emit<OpCode::LoadUniform>(Type::Void, {Operand::Uniform(offset, type)}));
    if (cache && bits >= 32) {
        builder.gprs[slot] = {value.node, {}, 0};
        builder.valid_gprs |= bit;
        builder.whole_gprs = static_cast<std::uint16_t>((builder.whole_gprs & ~bit) | (bits == 64 ? bit : 0));
    }
    return value;
}
void StorePC(Emitter& builder, StateLayout layout, Value pc) {
    builder.PublishRegisters();
    Require(builder.Emit<OpCode::StoreUniform>(Type::Void,
            {Operand::Uniform(layout.pc_offset, Type::U64), Operand::ValueOf(pc)}));
}
void PrepareFaultPC(Emitter& builder, StateLayout layout, u64 pc, TargetInfo target) {
    builder.PublishRegisters();
    if (target.Has(TargetFeature::FaultLocations))
        Require(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(pc, Type::U64)}));
    else StorePC(builder, layout, Constant(builder, pc));
}
void EndDispatch(Emitter& builder, u64 reason = 0) {
    Require(builder.EndBlock(reason ? Terminal::Return(Constant(builder, reason)) : Terminal::Dispatch()));
}
void WriteRegister(Emitter& builder, StateLayout layout, const _Operand& reg, Value value) {
    unsigned slot{};
    const auto offset = RegisterOffset(layout, reg.index, reg.size, slot);
    const auto type = ScalarType(reg.size);
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    if (builder.private_gprs) {
        const auto& producer = builder.Operations()[value.node.value];
        const bool canonical = IntegerType(producer.type) && producer.opcode != OpCode::BitCast &&
            producer.opcode != OpCode::GetResult && producer.opcode != OpCode::GetFlags &&
            !HasAnyEffect(EffectsFor(producer.opcode), Effect::Unknown | Effect::External | Effect::Control);
        // Same-width numeric values describe the whole register, including a
        // U32 write's implicit zero high half. A full overwrite kills an
        // unobserved partial without materializing its old parent.
        if (canonical && producer.type == type && (reg.size == 32 || reg.size == 64)) {
            builder.pending_gprs = static_cast<std::uint16_t>(builder.pending_gprs & ~bit);
            builder.gprs[slot] = {value.node, {}, 0};
            builder.valid_gprs |= bit;
            builder.dirty_gprs |= bit;
            builder.whole_gprs |= bit;
            return;
        }
        if (canonical && reg.size < 32 && (builder.valid_gprs & builder.whole_gprs & bit)) {
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
    if (reg.size < 32) builder.PublishGPRs(bit);
    else builder.pending_gprs = static_cast<std::uint16_t>(builder.pending_gprs & ~bit);
    builder.valid_gprs = static_cast<std::uint16_t>(builder.valid_gprs & ~bit);
    builder.dirty_gprs = static_cast<std::uint16_t>(builder.dirty_gprs & ~bit);
    builder.whole_gprs = static_cast<std::uint16_t>(builder.whole_gprs & ~bit);
    builder.gprs[slot].view = {};
    builder.gprs[slot].field_type = Type::Void;
    // StoreUniform reads the value's logical view, then stores the descriptor
    // width. U64 field + U32 input therefore zeroes the architectural high half,
    // including raw aliases, without birthing an explicit zero-extend node.
    Require(builder.Emit<OpCode::StoreUniform>(Type::Void,
            {Operand::Uniform(offset, reg.size == 32 ? Type::U64 : type),
             Operand::ValueOf(value, type)}));
}
bool Memory(const _Operand& operand) {
    return operand.type == O_SMEM || operand.type == O_MEM || operand.type == O_DISP;
}

// A one-shot memory consumer can borrow the final displacement instead of
// birthing an Add. Absolute/RIP addresses and ordinary Value users stay whole.
// Only LEA32 requests U32: its arithmetic and register reads need low32, while
// real memory, TLS and stack addresses continue to use the default U64 path.
Value Address(Emitter& builder, StateLayout layout, const _DInst& instruction,
              const _Operand& operand, u64 next_pc, TargetInfo target, Value stack_override = {},
              u64* final_disp = nullptr, Type result_type = Type::U64) {
    if (final_disp) *final_disp = 0;
    const bool narrow = result_type == Type::U32;
    const auto use = [&](Value value) { return Operand::ValueOf(value, narrow ? Type::U32 : Type::Void); };
    const auto literal = [&](u64 bits) { return Constant(builder, narrow ? static_cast<u32>(bits) : bits, result_type); };
    const auto add = [&](Value left, Operand right) {
        return narrow ? Require(builder.Emit<OpCode::Add>(Type::U32, {use(left), right})) : Add(builder, left, right);
    };
    const auto read = [&](unsigned reg) {
        if (reg == R_RSP && stack_override) return stack_override;
        if (!narrow) return ReadRegister(builder, layout, reg, 64);
        unsigned slot{};
        (void)RegisterOffset(layout, reg, 64, slot);
        // A captured U32 parent already supplies low32, even when it came
        // from a mere read rather than a whole-parent architectural write.
        // Borrow either cached width with an explicit numeric U32 use;
        // do not widen it or create a separate truncation before an ALU.
        if (builder.private_gprs && (builder.valid_gprs & (1u << slot))) {
            builder.MaterializeGPR(slot);
            return Value{builder.UnitId(), builder.gprs[slot].value};
        }
        return ReadRegister(builder, layout, REGS32_BASE + slot, 32);
    };
    const auto segment = instruction.opcode == I_LEA ? R_NONE : SEGMENT_GET(instruction.segment);
    const auto with_segment = [&](Value base) {
        if (segment != R_FS && segment != R_GS) return base;
        const auto offset = segment == R_FS ? layout.fs_base_offset : layout.gs_base_offset;
        if (offset > UINT32_MAX - 7) Unsupported();
        const auto segment_base = Require(builder.Emit<OpCode::LoadUniform>(Type::U64,
                {Operand::Uniform(offset, Type::U64)}));
        return base ? add(base, use(segment_base)) : segment_base;
    };
    const auto finish = [&](Value base) {
        base = with_segment(base);
        const auto displacement = narrow ? static_cast<u32>(instruction.disp) : instruction.disp;
        if (!base) return literal(displacement);
        if (final_disp) { *final_disp = instruction.disp; return base; }
        if (displacement) return add(base, Operand::Immediate(displacement, result_type));
        // A base-only LEA32 still writes a complete zero-high parent. Its
        // actual result must be U32, rather than merely label a raw U64 use.
        if (narrow && builder.Operations()[base.node.value].type != Type::U32)
            return Require(builder.Emit<OpCode::ZeroExtend32>(Type::U32, {use(base)}));
        return base;
    };
    if (operand.type == O_DISP) return finish({});
    if (operand.type == O_SMEM) {
        if (operand.index == R_RIP) return with_segment(literal(next_pc + instruction.disp));
        return finish(read(operand.index));
    }
    if (operand.type != O_MEM) Unsupported();
    Value address;
    if (instruction.base != R_NONE) address = read(instruction.base);
    if (operand.index != R_NONE) {
        auto index = read(operand.index);
        const auto scale = instruction.scale;
        if (scale > 1) {
            if (scale != 2 && scale != 4 && scale != 8) Unsupported();
            const auto shift = static_cast<std::uint8_t>(scale == 2 ? 1 : scale == 4 ? 2 : 3);
            if (address && target.Has(TargetFeature::ShiftedAddress)) {
                address = builder.AddressExpression(result_type,
                        {Operand::Expression(OperandOp::AddShift, 2, shift),
                         use(address), use(index)});
                return finish(address);
            }
            index = Require(builder.Emit<OpCode::LslImm>(narrow ? Type::U32 : Type::Void,
                    {use(index), Operand::Immediate(shift, Type::U8)}));
        }
        address = address ? add(address, use(index)) : index;
    }
    return finish(address);
}

u64 ImmediateBits(const _DInst& instruction, unsigned bits) {
    const u64 value = instruction.flags & FLAG_IMM_SIGNED
            ? static_cast<u64>(instruction.imm.sqword) : instruction.imm.qword;
    return bits == 64 ? value : value & ((u64{1} << bits) - 1);
}
Value LoadMemory(Emitter& builder, Type type, Value address, u64 displacement = 0) {
    if (displacement)
        return Require(builder.Emit<OpCode::LoadMemoryTSO>(type,
                {Operand::Expression(OperandOp::Add, 2), Operand::ValueOf(address),
                 Operand::Immediate(displacement, Type::U64)}));
    return Require(builder.Emit<OpCode::LoadMemoryTSO>(type, {Operand::ValueOf(address)}));
}
Value ReadData(Emitter& builder, StateLayout layout, const _DInst& instruction,
               const _Operand& source, u64 next_pc, TargetInfo target, bool materialize_address = false) {
    const auto type = ScalarType(source.size);
    if (source.type == O_REG) return ReadRegister(builder, layout, source.index, source.size);
    if (!Memory(source)) Unsupported();
    u64 displacement{};
    const auto address = Address(builder, layout, instruction, source, next_pc, target, {},
                                 materialize_address ? nullptr : &displacement);
    return LoadMemory(builder, type, address, displacement);
}
template<OpCode op = OpCode::StoreMemoryTSO>
void StoreMemory(Emitter& builder, Value address, Value value, unsigned bits, u64 displacement = 0) {
    const auto type = bits == 128 ? Type::V128 : ScalarType(bits);
    if (displacement)
        Require(builder.Emit<op>(Type::Void,
                {Operand::Expression(OperandOp::Add, 2), Operand::ValueOf(address),
                 Operand::Immediate(displacement, Type::U64), Operand::ValueOf(value, type)}));
    else
        Require(builder.Emit<op>(Type::Void,
                {Operand::ValueOf(address), Operand::ValueOf(value, type)}));
}

void Move(Emitter& builder, StateLayout layout, const _DInst& instruction, u64 pc, u64 next_pc,
          TargetInfo target) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    const auto bits = destination.size;
    const auto type = ScalarType(bits);
    if (instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE ||
        (destination.type != O_REG && !Memory(destination)) ||
        (source.type != O_REG && source.type != O_IMM && !Memory(source)) ||
        (Memory(destination) && Memory(source))) Unsupported();
    if (source.type != O_IMM && source.size != bits) Unsupported();
    if (Memory(destination) || Memory(source)) PrepareFaultPC(builder, layout, pc, target);
    Value value;
    if (source.type == O_IMM) {
        value = Constant(builder, ImmediateBits(instruction, bits), type);
    } else value = ReadData(builder, layout, instruction, source, next_pc, target);
    if (destination.type == O_REG) WriteRegister(builder, layout, destination, value);
    else {
        u64 displacement{};
        const auto address = Address(builder, layout, instruction, destination, next_pc, target, {}, &displacement);
        StoreMemory(builder, address, value, bits, displacement);
    }
}

void ExtendedMove(Emitter& builder, StateLayout layout, const _DInst& instruction,
                   u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    if (destination.type != O_REG || (source.type != O_REG && !Memory(source)) ||
        instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE ||
        (destination.size != 16 && destination.size != 32 && destination.size != 64)) Unsupported();
    if (instruction.opcode == I_MOVSXD) {
        if (source.size != 32 || destination.size < 32) Unsupported();
    } else if ((source.size != 8 && source.size != 16) || source.size >= destination.size) Unsupported();
    if (Memory(source)) PrepareFaultPC(builder, layout, pc, target);
    const auto source_value = ReadData(builder, layout, instruction, source, next_pc, target);
    // ReadData births a numeric load, whose carrier is already zero above the
    // source width. The wider store view needs no extra MOVZX value node.
    const auto value = instruction.opcode == I_MOVZX
            ? source_value
            : Require(builder.Emit<OpCode::SignExtend>(ScalarType(destination.size),
                    {Operand::ValueOf(source_value, ScalarType(source.size))}));
    WriteRegister(builder, layout, destination, value);
}

void OneOperand(const _DInst& instruction) {
    if (instruction.ops[1].type != O_NONE || instruction.ops[2].type != O_NONE ||
        instruction.ops[3].type != O_NONE) Unsupported();
}
unsigned StackBits(const _DInst& instruction) {
    return FLAG_GET_OPSIZE(instruction.flags) == Decode16Bits ? 16 : 64;
}
void WriteStack(Emitter& builder, StateLayout layout, Value stack) {
    WriteRegister(builder, layout, _Operand{O_REG, R_RSP, 64}, stack);
}
void Push(Emitter& builder, StateLayout layout, const _DInst& instruction, u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& source = instruction.ops[0];
    const auto bits = StackBits(instruction);
    if ((source.type != O_IMM && source.type != O_REG && !Memory(source)) ||
        (source.type != O_IMM && source.size != bits)) Unsupported();
    // PUSH RSP and [RSP+...] read the original stack before decrementing it.
    const auto value = source.type == O_IMM ? Constant(builder, ImmediateBits(instruction, bits), ScalarType(bits))
            : ReadData(builder, layout, instruction, source, next_pc, target, true);
    const auto stack = ReadRegister(builder, layout, R_RSP, 64);
    const auto changed = Add(builder, stack, Operand::Immediate(u64{} - bits / 8, Type::U64));
    StoreMemory(builder, changed, value, bits);
    WriteStack(builder, layout, changed);
}
void Pop(Emitter& builder, StateLayout layout, const _DInst& instruction, u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& destination = instruction.ops[0];
    const auto bits = StackBits(instruction);
    if ((destination.type != O_REG && !Memory(destination)) || destination.size != bits) Unsupported();
    const auto stack = ReadRegister(builder, layout, R_RSP, 64);
    const auto value = Require(builder.Emit<OpCode::LoadMemoryTSO>(ScalarType(bits), {Operand::ValueOf(stack)}));
    const auto changed = Add(builder, stack, Operand::Immediate(bits / 8, Type::U64));
    if (Memory(destination)) {
        // POP [RSP+...] addresses through the incremented stack, but publishes
        // neither the stack nor its data result before the store succeeds.
        const auto address = Address(builder, layout, instruction, destination, next_pc, target, changed);
        StoreMemory(builder, address, value, bits);
    }
    WriteStack(builder, layout, changed);
    if (destination.type == O_REG) WriteRegister(builder, layout, destination, value);
}
void Leave(Emitter& builder, StateLayout layout, const _DInst& instruction) {
    if (instruction.ops[0].type != O_NONE) Unsupported();
    const auto bits = StackBits(instruction);
    const auto base = ReadRegister(builder, layout, R_RBP, 64);
    const auto value = Require(builder.Emit<OpCode::LoadMemoryTSO>(ScalarType(bits), {Operand::ValueOf(base)}));
    const auto changed = Add(builder, base, Operand::Immediate(bits / 8, Type::U64));
    WriteStack(builder, layout, changed);
    WriteRegister(builder, layout, _Operand{O_REG, static_cast<std::uint8_t>(bits == 64 ? R_RBP : R_BP),
                                        static_cast<std::uint16_t>(bits)}, value);
}

Value TransferTarget(Emitter& builder, StateLayout layout, const _DInst& instruction,
                     u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& destination = instruction.ops[0];
    if (destination.type == O_PC && (destination.size == 8 || destination.size == 32))
        return Constant(builder, next_pc + instruction.imm.addr);
    if ((destination.type != O_REG && !Memory(destination)) || destination.size != 64) Unsupported();
    return ReadData(builder, layout, instruction, destination, next_pc, target, true);
}

bool LockedMemoryForm(const _DInst& instruction) {
    if (instruction.opcode == I_XCHG)
        return Memory(instruction.ops[0]) || Memory(instruction.ops[1]);
    if (!Memory(instruction.ops[0])) return false;
    // Prefix permission only. Each emitter checks its actual operand shape
    // and width once, along the same path used without LOCK.
    switch (instruction.opcode) {
        case I_CMPXCHG: case I_XADD: case I_INC: case I_DEC: case I_NEG: case I_NOT:
        case I_ADD: case I_SUB: case I_ADC: case I_SBB: case I_AND: case I_OR: case I_XOR:
            return true;
        default: return false;
    }
}
bool AllowedPrefixes(const _DInst& instruction, std::span<const std::uint8_t> bytes) {
    const bool mandatory_f3 = instruction.opcode == I_MOVDQU || instruction.opcode == I_MOVQ ||
            instruction.opcode == I_POPCNT || instruction.opcode == I_TZCNT || instruction.opcode == I_CVTDQ2PD;
    const bool mandatory_f2 = instruction.opcode == I_PSHUFLW || instruction.opcode == I_CRC32 ||
            instruction.opcode == I_MOVSD || instruction.opcode == I_CVTSI2SD ||
            instruction.opcode == I_ADDSD || instruction.opcode == I_SUBSD || instruction.opcode == I_MULSD ||
            instruction.opcode == I_DIVSD || instruction.opcode == I_CVTTSD2SI;
    if (((instruction.flags & FLAG_LOCK) && !LockedMemoryForm(instruction)) || ((instruction.flags & FLAG_REP) && !mandatory_f3) ||
        ((instruction.flags & FLAG_REPNZ) && !mandatory_f2) ||
        FLAG_GET_ADDRSIZE(instruction.flags) != Decode64Bits)
        return false;
    for (const auto byte : bytes.first(instruction.size)) {
        if ((byte == 0xf0 && !LockedMemoryForm(instruction)) || (byte == 0xf2 && !mandatory_f2) || (byte == 0xf3 && !mandatory_f3) ||
            byte == 0x67)
            return false;
        if (byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e || byte == 0x64 || byte == 0x65 ||
            byte == 0x66 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 ||
            (byte >= 0x40 && byte <= 0x4f)) continue;
        return byte != 0xc4 && byte != 0xc5 && byte != 0x62;
    }
    return false;
}

bool ValidRequest(std::size_t blocks, Scope scope, StateLayout layout, TargetInfo target) {
    if (!blocks || blocks > BlockId::invalid ||
        (scope != Scope::Block && scope != Scope::Function) ||
        (scope == Scope::Block && blocks != 1) ||
        static_cast<unsigned>(target.mode) > static_cast<unsigned>(TargetMode::Unknown) ||
        layout.stride < 8 || layout.pc_offset > std::numeric_limits<u32>::max() - 7 ||
        u64{layout.gprs_offset} + u64{15} * layout.stride > std::numeric_limits<u32>::max() - 7)
        return false;
    if (layout.fprs_offset != UINT32_MAX &&
        (layout.fprs_offset > UINT32_MAX - 255 ||
         (u64{layout.fprs_offset} < u64{layout.pc_offset} + 8 && layout.pc_offset < u64{layout.fprs_offset} + 256))) return false;
    for (unsigned reg = 0; reg < 16; ++reg) {
        const u64 offset = u64{layout.gprs_offset} + u64{reg} * layout.stride;
        if (offset < u64{layout.pc_offset} + 8 && layout.pc_offset < offset + 8) return false;
        if (layout.fprs_offset != UINT32_MAX && offset < u64{layout.fprs_offset} + 256 &&
            layout.fprs_offset < offset + 8) return false;
    }
    return true;
}
bool ValidSources(std::span<const BlockWindow> windows) {
    if (windows.empty()) return false;
    for (std::size_t i = 0; i < windows.size(); ++i)
        if (windows[i].bytes.empty() || !windows[i].bytes.data() ||
            windows[i].bytes.size() > std::numeric_limits<u64>::max() - windows[i].pc ||
            (i && windows[i - 1].pc >= windows[i].pc)) return false;
    return true;
}

bool Conditional(unsigned opcode) {
    switch (opcode) {
        case I_JO: case I_JNO: case I_JB: case I_JAE: case I_JZ: case I_JNZ:
        case I_JBE: case I_JA: case I_JS: case I_JNS: case I_JP: case I_JNP:
        case I_JL: case I_JGE: case I_JLE: case I_JG: return true;
        default: return false;
    }
}
bool Direct(const _DInst& instruction) {
    return instruction.ops[0].type == O_PC && instruction.ops[1].type == O_NONE &&
           instruction.ops[2].type == O_NONE && instruction.ops[3].type == O_NONE &&
           (instruction.ops[0].size == 8 || instruction.ops[0].size == 32);
}
_Operand Gpr(unsigned slot, unsigned bits) {
    const auto base = bits == 64 ? REGS64_BASE : bits == 32 ? REGS32_BASE : bits == 16 ? REGS16_BASE : REGS8_BASE;
    const auto reg = bits == 8 && slot >= 4 && slot < 8 ? REGS8_REX_BASE + slot - 4 : base + slot;
    return {O_REG, static_cast<std::uint8_t>(reg), static_cast<std::uint16_t>(bits)};
}
std::array<_Operand, 2> SynchronizationOperands(StateLayout layout, const _DInst& instruction,
                                              std::span<const std::uint8_t> bytes) {
    std::array operands{instruction.ops[0], instruction.ops[1]};
    unsigned bits = operands[0].size;
    // The trusted decoder can report 32-bit operands for these 66 forms.
    // Restore their actual scalar width from the already supplied bytes.
    if (bits != 8 && bits != 64)
        for (const auto byte : bytes) {
            if (byte == 0x0f) break;
            if (byte == 0x66) bits = 16;
        }
    ScalarType(bits);
    for (auto& operand : operands) if (operand.size != bits) {
        if (bits != 16 || operand.size != 32) Unsupported();
        if (operand.type == O_REG) {
            unsigned slot{}; RegisterOffset(layout, operand.index, operand.size, slot);
            operand.index = Gpr(slot, bits).index;
        }
        operand.size = static_cast<std::uint16_t>(bits);
    }
    return operands;
}
void Complement(Emitter& builder, StateLayout layout, const _DInst& instruction,
                  u64 pc, u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& destination = instruction.ops[0];
    if (destination.type != O_REG && !Memory(destination)) Unsupported();
    const auto type = ScalarType(destination.size);
    Value address;
    if (Memory(destination)) {
        PrepareFaultPC(builder, layout, pc, target);
        address = Address(builder, layout, instruction, destination, next_pc, target);
    }
    if (instruction.flags & FLAG_LOCK) {
        if (!address) Unsupported();
        const auto ones = Constant(builder, UINT64_MAX >> (64 - destination.size), type), carry = Constant(builder, 0, Type::U8);
        Require(builder.Emit<OpCode::AtomicRMW>(type,
                {Operand::Immediate(static_cast<u64>(AtomicRMWOp::Xor), Type::U8), Operand::ValueOf(address),
                 Operand::ValueOf(ones), Operand::ValueOf(carry)}));
        return; // NOT never writes flags.
    }
    const auto old = address ? Require(builder.Emit<OpCode::LoadMemoryTSO>(type, {Operand::ValueOf(address)}))
            : ReadRegister(builder, layout, destination.index, destination.size);
    const auto result = Require(builder.Emit<OpCode::Not>(type, {Operand::ValueOf(old), Operand::ValueOf(old)}));
    if (address) StoreMemory(builder, address, result, destination.size);
    else WriteRegister(builder, layout, destination, result);
}
void Exchange(Emitter& builder, StateLayout layout, const _DInst& instruction,
                u64 pc, u64 next_pc, TargetInfo target) {
    const auto& left = instruction.ops[0]; const auto& right = instruction.ops[1];
    if (instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE || left.size != right.size ||
        (left.type != O_REG && !Memory(left)) || (right.type != O_REG && !Memory(right)) ||
        (Memory(left) && Memory(right))) Unsupported();
    const auto type = ScalarType(left.size);
    if (Memory(left) || Memory(right)) {
        const auto& memory = Memory(left) ? left : right;
        const auto& reg = Memory(left) ? right : left;
        PrepareFaultPC(builder, layout, pc, target);
        const auto address = Address(builder, layout, instruction, memory, next_pc, target);
        const auto value = ReadRegister(builder, layout, reg.index, reg.size);
        // XCHG with memory is implicitly locked, including without an F0 prefix.
        const auto old = Require(builder.Emit<OpCode::AtomicExchange>(type,
                {Operand::ValueOf(address), Operand::ValueOf(value, type)}));
        WriteRegister(builder, layout, reg, old);
    } else {
        const auto a = ReadRegister(builder, layout, left.index, left.size);
        const auto b = ReadRegister(builder, layout, right.index, right.size);
        WriteRegister(builder, layout, left, b);
        WriteRegister(builder, layout, right, a);
    }
}
Value Widen(Emitter& builder, Value value, unsigned bits, bool sign) {
    if (bits == 64) return value;
    return sign ? Require(builder.Emit<OpCode::SignExtend>(Type::U64, {Operand::ValueOf(value, ScalarType(bits))}))
            : Require(builder.Emit<OpCode::ZeroExtend64>(Type::Void, {Operand::ValueOf(value, ScalarType(bits))}));
}
void ExtendAccumulator(Emitter& builder, StateLayout layout, const _DInst& instruction) {
    unsigned source_bits{}, destination_bits{};
    bool high{};
    switch (instruction.opcode) {
        case I_CBW: source_bits = 8; destination_bits = 16; break;
        case I_CWDE: source_bits = 16; destination_bits = 32; break;
        case I_CDQE: source_bits = 32; destination_bits = 64; break;
        case I_CWD: source_bits = destination_bits = 16; high = true; break;
        case I_CDQ: source_bits = destination_bits = 32; high = true; break;
        case I_CQO: source_bits = destination_bits = 64; high = true; break;
        default: Unsupported();
    }
    const auto source = ReadRegister(builder, layout, Gpr(0, source_bits).index, source_bits);
    const auto result = high ? Require(builder.Emit<OpCode::AsrImm>(Type::U64,
            {Operand::ValueOf(Widen(builder, source, source_bits, true)), Operand::Immediate(63, Type::U8)}))
            : Require(builder.Emit<OpCode::SignExtend>(ScalarType(destination_bits), {Operand::ValueOf(source)}));
    WriteRegister(builder, layout, Gpr(high ? 2 : 0, destination_bits), result);
}
struct PendingFlags {
    NodeId producer;
    FlagProfile profile{FlagProfile::None};
    // Only the still-unobserved bits of this one real producer remain pending.
    // Published bits live in its actual metadata, not a second demand table.
    u64 outputs{FlagBits(Flag::All)};
    struct Recipe {
        Value left, right, result, high;
        unsigned bits{}, constant{}, opcode{};
        bool wide_product{};
    } recipe;

    void PublishShift(Emitter& builder);
    void PublishMultiply(Emitter& builder);
    void Observe(Emitter& builder, u64 needed) {
        if (recipe.result && needed) {
            const bool multiply = recipe.opcode == I_MUL || recipe.opcode == I_IMUL;
            if (!multiply || (needed & FlagBits(Flag::Carry | Flag::Overflow))) {
                if (multiply) PublishMultiply(builder);
                else PublishShift(builder);
                recipe = {};
            }
        }
        if (!producer) return;
        const auto publish = outputs & needed;
        if (publish) {
            const auto metadata = builder.Operations()[producer.value].Metadata();
            Require(builder.SetMetadata(producer, {metadata.flag_outputs | publish, profile, false}));
            outputs &= ~publish;
        }
        if (!outputs) producer = {};
    }
    void Observe(Emitter& builder, const _DInst& instruction) {
        // The actual trusted decoder record already describes this consumer's
        // EFLAGS inputs. Reuse it rather than duplicate its condition table.
        const auto flags = instruction.testedFlagsMask;
        Observe(builder, (flags & D_CF ? FlagBits(Flag::Carry) : 0) |
                (flags & D_PF ? FlagBits(Flag::Parity) : 0) |
                (flags & D_AF ? FlagBits(Flag::AuxiliaryCarry) : 0) |
                (flags & D_ZF ? FlagBits(Flag::Zero) : 0) |
                (flags & D_SF ? FlagBits(Flag::Negate) : 0) |
                (flags & D_OF ? FlagBits(Flag::Overflow) : 0));
    }
    void Flush(Emitter& builder) { Observe(builder, FlagBits(Flag::All)); }
};

void BitScan(Emitter& builder, StateLayout layout, const _DInst& instruction,
             std::span<const std::uint8_t> bytes, PendingFlags& pending,
             u64 pc, u64 next_pc, TargetInfo target) {
    if (instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE) Unsupported();
    const auto operands = SynchronizationOperands(layout, instruction, bytes);
    const auto& destination = operands[0];
    const auto& source = operands[1];
    const auto bits = destination.size;
    if (destination.type != O_REG || (source.type != O_REG && !Memory(source)) ||
        (bits != 16 && bits != 32 && bits != 64) || source.size != bits) Unsupported();
    if (Memory(source)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
    }
    const auto value = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto count_type = bits == 64 ? Type::U64 : Type::U32;
    const auto input = bits == 16 ? Require(builder.Emit<OpCode::ZeroExtend32>(Type::U32,
            {Operand::ValueOf(value, Type::U16)})) : value;
    if (instruction.opcode == I_TZCNT) {
        const auto carry = Require(builder.Emit<OpCode::TestZero>(Type::Void, {Operand::ValueOf(input)}));
        // A set sentinel above the word leaves every nonzero word's trailing
        // count intact, and makes its zero result 16 rather than CTZ32's 32.
        const auto counted = bits == 16 ? Require(builder.Emit<OpCode::Or>(Type::U32,
                {Operand::ValueOf(input), Operand::Immediate(1u << 16, Type::U32)})) : input;
        const auto result = bits == 64
                ? Require(builder.Emit<OpCode::CountTrailingZeros64>(Type::U64, {Operand::ValueOf(counted)}))
                : Require(builder.Emit<OpCode::CountTrailingZeros32>(Type::U32, {Operand::ValueOf(counted)}));
        WriteRegister(builder, layout, destination, result);
        Require(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(carry)}));
        const auto flags = Require(builder.Emit<OpCode::Sub>(count_type,
                {Operand::ValueOf(result), Operand::Immediate(0, count_type)}));
        pending = {flags.node, FlagProfile::ArithmeticCanonicalCarry, FlagBits(Flag::Zero), {}};
        return;
    }
    // Consume the old source before the count can take its destination pin.
    const auto flags = Require(builder.Emit<OpCode::Sub>(ScalarType(bits),
            {Operand::ValueOf(value), Operand::Immediate(0, ScalarType(bits))}));
    const bool reverse = instruction.opcode == I_BSR;
    auto result = bits == 64
            ? reverse ? Require(builder.Emit<OpCode::CountLeadingZeros64>(Type::U64, {Operand::ValueOf(input)}))
                      : Require(builder.Emit<OpCode::CountTrailingZeros64>(Type::U64, {Operand::ValueOf(input)}))
            : reverse ? Require(builder.Emit<OpCode::CountLeadingZeros32>(Type::U32, {Operand::ValueOf(input)}))
                      : Require(builder.Emit<OpCode::CountTrailingZeros32>(Type::U32, {Operand::ValueOf(input)}));
    if (reverse)
        result = Require(builder.Emit<OpCode::Xor>(count_type,
                {Operand::ValueOf(result), Operand::Immediate(bits == 64 ? 63 : 31, count_type)}));
    WriteRegister(builder, layout, destination, result);
    // ZF tests the original source, not the scan index. A zero source leaves
    // the destination and the other arithmetic flags architecturally undefined.
    pending = {flags.node, FlagProfile::ArithmeticCanonicalCarry, FlagBits(Flag::Zero), {}};
}

void PopulationCount(Emitter& builder, StateLayout layout, const _DInst& instruction,
                     std::span<const std::uint8_t> bytes, PendingFlags& pending,
                     u64 pc, u64 next_pc, TargetInfo target) {
    if (instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE) Unsupported();
    const auto operands = SynchronizationOperands(layout, instruction, bytes);
    const auto& destination = operands[0];
    const auto& source = operands[1];
    const auto bits = destination.size;
    if (destination.type != O_REG || (source.type != O_REG && !Memory(source)) ||
        (bits != 16 && bits != 32 && bits != 64) || source.size != bits) Unsupported();
    if (Memory(source)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
    }
    auto input = ReadData(builder, layout, instruction, source, next_pc, target);
    // PopCount consumes the whole raw container, so normalize a narrow source
    // explicitly rather than merely change the view of a wider carrier.
    if (bits != 64)
        input = Require(builder.Emit<OpCode::ZeroExtend64>(Type::U64,
                {Operand::ValueOf(input, ScalarType(bits))}));
    const auto result = Require(builder.Emit<OpCode::PopCount>(Type::U64, {Operand::ValueOf(input)}));
    WriteRegister(builder, layout, destination, result);
    Require(builder.Emit<OpCode::ClearFlags>(Type::Void,
            {Operand::Flags(FlagBits(Flag::All) & ~FlagBits(Flag::Zero))}));
    // A zero count is exactly a zero source; only ZF remains deferred.
    const auto flags = Require(builder.Emit<OpCode::Sub>(Type::U64,
            {Operand::ValueOf(result), Operand::Immediate(0, Type::U64)}));
    pending = {flags.node, FlagProfile::ArithmeticCanonicalCarry, FlagBits(Flag::Zero), {}};
}

void Crc32(Emitter& builder, StateLayout layout, const _DInst& instruction,
           std::span<const std::uint8_t> bytes, PendingFlags& pending,
           u64 pc, u64 next_pc, TargetInfo target) {
    auto destination = instruction.ops[0], source = instruction.ops[1];
    if (instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE || destination.type != O_REG ||
        (destination.size != 16 && destination.size != 32 && destination.size != 64) ||
        (source.type != O_REG && !Memory(source))) Unsupported();
    ScalarType(source.size);
    // CRC32's destination is always r32/r64. In particular, 66 selects
    // word data but never an r16 destination. Only the effective REX.W
    // widens the destination (and the non-byte data form).
    bool word{};
    std::uint8_t rex{};
    for (const auto byte : bytes) {
        if (byte == 0x0f) break;
        if (byte >= 0x40 && byte <= 0x4f) rex = byte;
        else { rex = 0; if (byte == 0x66) word = true; }
    }
    unsigned destination_slot{};
    RegisterOffset(layout, destination.index, destination.size, destination_slot);
    const unsigned destination_bits = rex & 8 ? 64 : 32;
    // Both encoded destinations zero-extend the same U32 result into the full GPR.
    destination = Gpr(destination_slot, 32);
    const unsigned bits = source.size == 8 ? 8 : destination_bits == 64 ? 64 : word ? 16 : 32;
    if (source.size != bits) {
        if (source.type == O_REG) {
            unsigned slot{};
            RegisterOffset(layout, source.index, source.size, slot);
            source = Gpr(slot, bits);
        } else source.size = static_cast<std::uint16_t>(bits);
    }
    if (Memory(source)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
    }
    const auto accumulator = ReadRegister(builder, layout, Gpr(destination_slot, 32).index, 32);
    const auto data = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto result = Require(builder.Emit<OpCode::Crc32c>(Type::U32,
            {Operand::ValueOf(accumulator, Type::U32), Operand::ValueOf(data, ScalarType(bits)),
             Operand::Immediate(bits, Type::U8)}));
    WriteRegister(builder, layout, destination, result);
}

void CompareExchange(Emitter& builder, StateLayout layout, const _DInst& instruction,
                     std::span<const std::uint8_t> bytes, PendingFlags& pending,
                     u64 pc, u64 next_pc, TargetInfo target) {
    if (instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE ||
        (instruction.ops[0].type != O_REG && !Memory(instruction.ops[0])) || instruction.ops[1].type != O_REG) Unsupported();
    const auto operands = SynchronizationOperands(layout, instruction, bytes);
    const auto& destination = operands[0]; const auto& source = operands[1];
    const auto bits = destination.size;
    const auto type = ScalarType(bits);
    const bool locked = instruction.flags & FLAG_LOCK;
    Value address;
    if (Memory(destination)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
        address = Address(builder, layout, instruction, destination, next_pc, target);
    }
    // Capture complete 32-bit parents before any conditional writes. Their
    // high halves survive a non-written branch, including operand aliases.
    const auto accumulator = ReadRegister(builder, layout, Gpr(0, bits == 32 ? 64 : bits).index, bits == 32 ? 64 : bits);
    const auto desired = ReadRegister(builder, layout, source.index, bits);
    Value old;
    unsigned destination_slot{};
    if (address)
        old = locked ? Require(builder.Emit<OpCode::CompareAndSwap>(type,
                {Operand::ValueOf(address), Operand::ValueOf(accumulator, type), Operand::ValueOf(desired, type)}))
                     : Require(builder.Emit<OpCode::LoadMemoryTSO>(type, {Operand::ValueOf(address)}));
    else {
        RegisterOffset(layout, destination.index, bits, destination_slot);
        old = bits == 32 ? ReadRegister(builder, layout, Gpr(destination_slot, 64).index, 64)
                         : ReadRegister(builder, layout, destination.index, bits);
    }
    const auto compare = [&] {
        return Require(builder.Emit<OpCode::Sub>(type,
                {Operand::ValueOf(accumulator, type), Operand::ValueOf(old, type)}));
    };
    auto flags = compare();
    const auto equal = Require(builder.Emit<OpCode::TestZero>(Type::Bool, {Operand::ValueOf(flags)}));
    const auto select = [&](Type result_type, Value yes, Value no) {
        return Require(builder.Emit<OpCode::Select>(result_type,
                {Operand::ValueOf(equal), Operand::ValueOf(yes), Operand::ValueOf(no)}));
    };
    if (address && !locked) {
        // Keep ordinary unaligned memory legal, including the failed compare's
        // write cycle. Pure selection restores guest NZCV before this store.
        StoreMemory(builder, address, select(type, desired, old), bits);
        flags = compare(); // Old flags remain live until the store succeeds.
    }
    if (bits == 32) {
        const auto observed64 = Require(builder.Emit<OpCode::ZeroExtend64>(Type::U64,
                {Operand::ValueOf(old, Type::U32)}));
        WriteRegister(builder, layout, Gpr(0, 64), select(Type::U64, accumulator, observed64));
        if (!address) {
            const auto desired64 = Require(builder.Emit<OpCode::ZeroExtend64>(Type::U64,
                    {Operand::ValueOf(desired, Type::U32)}));
            WriteRegister(builder, layout, Gpr(destination_slot, 64), select(Type::U64, desired64, old));
        }
    } else {
        // Partial writes preserve other lanes. Accumulator first also handles
        // AL/AH aliases; a destination equal to the accumulator wins last.
        WriteRegister(builder, layout, Gpr(0, bits), select(type, accumulator, old));
        if (!address) WriteRegister(builder, layout, destination, select(type, desired, old));
    }
    pending = {flags.node, FlagProfile::ArithmeticCanonicalCarry, FlagBits(Flag::All), {}};
}

void ExchangeAdd(Emitter& builder, StateLayout layout, const _DInst& instruction,
                 std::span<const std::uint8_t> bytes, PendingFlags& pending,
                 u64 pc, u64 next_pc, TargetInfo target) {
    if (instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE ||
        (instruction.ops[0].type != O_REG && !Memory(instruction.ops[0])) || instruction.ops[1].type != O_REG) Unsupported();
    const auto operands = SynchronizationOperands(layout, instruction, bytes);
    const auto& destination = operands[0]; const auto& source = operands[1];
    const auto type = ScalarType(destination.size);
    Value address;
    if (Memory(destination)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
        address = Address(builder, layout, instruction, destination, next_pc, target);
    }
    const auto addend = ReadRegister(builder, layout, source.index, source.size);
    const bool locked = instruction.flags & FLAG_LOCK;
    const auto old = address ? (locked
            ? Require(builder.Emit<OpCode::AtomicFetchAdd>(type, {Operand::ValueOf(address), Operand::ValueOf(addend)}))
            : Require(builder.Emit<OpCode::LoadMemoryTSO>(type, {Operand::ValueOf(address)})))
            : ReadRegister(builder, layout, destination.index, destination.size);
    const auto calculate = [&] {
        return Require(builder.Emit<OpCode::Add>(type, {Operand::ValueOf(old, type), Operand::ValueOf(addend, type)}));
    };
    const auto result = calculate();
    auto flags = result;
    if (address && !locked) {
        StoreMemory(builder, address, result, destination.size);
        flags = calculate(); // A fault must still observe the preceding flags.
    }
    // Source first, destination last: XADD eax,eax and AL/AH overlap retain
    // the architectural result rather than the intermediate old-value copy.
    WriteRegister(builder, layout, source, old);
    if (!address) WriteRegister(builder, layout, destination, result);
    pending = {flags.node, FlagProfile::ArithmeticCanonicalCarry, FlagBits(Flag::All), {}};
}

void Multiply(Emitter& builder, StateLayout layout, const _DInst& instruction,
                PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target) {
    const bool sign = instruction.opcode == I_IMUL;
    const auto& destination = instruction.ops[0];
    const bool implicit = instruction.ops[1].type == O_NONE;
    const auto bits = destination.size;
    ScalarType(bits);
    if (instruction.ops[3].type != O_NONE || (!sign && !implicit)) Unsupported();
    Value left, right;
    if (implicit) {
        if ((destination.type != O_REG && !Memory(destination)) || instruction.ops[2].type != O_NONE) Unsupported();
        if (Memory(destination)) PrepareFaultPC(builder, layout, pc, target);
        left = ReadRegister(builder, layout, Gpr(0, bits).index, bits);
        right = ReadData(builder, layout, instruction, destination, next_pc, target);
    } else {
        const auto& source = instruction.ops[1]; const auto& immediate = instruction.ops[2];
        if (destination.type != O_REG || bits == 8 || source.size != bits ||
            (source.type != O_REG && !Memory(source)) ||
            (immediate.type != O_NONE && immediate.type != O_IMM)) Unsupported();
        if (Memory(source)) PrepareFaultPC(builder, layout, pc, target);
        if (immediate.type == O_IMM) {
            left = ReadData(builder, layout, instruction, source, next_pc, target);
            right = Constant(builder, ImmediateBits(instruction, bits), ScalarType(bits));
        } else {
            left = ReadRegister(builder, layout, destination.index, bits);
            right = ReadData(builder, layout, instruction, source, next_pc, target);
        }
    }
    // Explicit IMUL needs only its N-bit low product. Widening and signed
    // overflow work are flags-only; implicit AX / high:low outputs retain
    // their actual wider data product.
    const bool wide_product = implicit || bits == 64;
    Value low, high;
    if (wide_product) {
        const auto a = Widen(builder, left, bits, sign), b = Widen(builder, right, bits, sign);
        low = Require(builder.Emit<OpCode::Mul>(Type::U64, {Operand::ValueOf(a), Operand::ValueOf(b)}));
        if (implicit && bits != 8)
            high = bits == 64 ? Require(builder.Emit<OpCode::MulHigh>(Type::U64,
                    {Operand::ValueOf(a), Operand::ValueOf(b), Operand::Immediate(sign, Type::U8)}))
                    : Require(builder.Emit<OpCode::BitExtract>(Type::U64,
                            {Operand::ValueOf(low), Operand::Immediate(bits, Type::U8), Operand::Immediate(bits, Type::U8)}));
    } else {
        const auto type = ScalarType(bits);
        low = Require(builder.Emit<OpCode::Mul>(type,
                {Operand::ValueOf(left, type), Operand::ValueOf(right, type)}));
    }
    if (!implicit) WriteRegister(builder, layout, destination, low);
    else if (bits == 8) WriteRegister(builder, layout, Gpr(0, 16), low);
    else {
        WriteRegister(builder, layout, Gpr(0, bits), low);
        WriteRegister(builder, layout, Gpr(2, bits), high);
    }
    pending.recipe = {left, right, low, high, bits, 0, instruction.opcode, wide_product};
}

void PendingFlags::PublishMultiply(Emitter& builder) {
    const auto& [left, right, low, data_high, bits, constant, opcode, wide_product] = recipe;
    const bool sign = opcode == I_IMUL;
    Value different;
    if (sign && bits < 64) {
        auto full = low;
        if (!wide_product) {
            const auto a = Widen(builder, left, bits, true), b = Widen(builder, right, bits, true);
            full = Require(builder.Emit<OpCode::Mul>(Type::U64, {Operand::ValueOf(a), Operand::ValueOf(b)}));
        }
        const auto expected = Widen(builder, low, bits, true);
        different = Require(builder.Emit<OpCode::Xor>(Type::U64,
                {Operand::ValueOf(full), Operand::ValueOf(expected)}));
    } else {
        auto high = data_high;
        if (!high)
            high = bits == 64 ? Require(builder.Emit<OpCode::MulHigh>(Type::U64,
                    {Operand::ValueOf(left), Operand::ValueOf(right), Operand::Immediate(sign, Type::U8)}))
                    : Require(builder.Emit<OpCode::BitExtract>(Type::U64,
                            {Operand::ValueOf(low), Operand::Immediate(bits, Type::U8), Operand::Immediate(bits, Type::U8)}));
        different = high;
        if (sign) {
            const auto expected = Require(builder.Emit<OpCode::AsrImm>(Type::U64,
                    {Operand::ValueOf(low), Operand::Immediate(63, Type::U8)}));
            different = Require(builder.Emit<OpCode::Xor>(Type::U64,
                    {Operand::ValueOf(high), Operand::ValueOf(expected)}));
        }
    }
    const auto overflow = Require(builder.Emit<OpCode::TestNotZero>(Type::Void, {Operand::ValueOf(different)}));
    Require(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(overflow)}));
    Require(builder.Emit<OpCode::SetOverflow>(Type::Void, {Operand::ValueOf(overflow)}));
}
void Divide(Emitter& builder, StateLayout layout, const _DInst& instruction,
              u64 pc, u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& source = instruction.ops[0];
    if (source.type != O_REG && !Memory(source)) Unsupported();
    const auto bits = source.size;
    const auto type = ScalarType(bits);
    PrepareFaultPC(builder, layout, pc, target);
    // Division's C++ helper faults outside the JIT fault-location span.
    if (target.Has(TargetFeature::FaultLocations)) StorePC(builder, layout, Constant(builder, pc));
    const auto divisor = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto low = ReadRegister(builder, layout, Gpr(0, bits).index, bits);
    const auto high = ReadRegister(builder, layout, bits == 8 ? unsigned(R_AH) : Gpr(2, bits).index, bits);
    // Same-width halves and divisor define the actual 2N/N operation. The
    // backend checks division by zero and quotient overflow at N, including
    // arbitrary high halves; there is no high==0 shortcut.
    const auto quotient = Require(builder.Emit<OpCode::Div128>(Type::Void,
            {Operand::ValueOf(high, type), Operand::ValueOf(low, type), Operand::ValueOf(divisor, type),
             Operand::Immediate(instruction.opcode == I_IDIV, Type::U8)}));
    const auto remainder = Require(builder.Emit<OpCode::Div128Remainder>(Type::Void, {Operand::ValueOf(quotient)}));
    WriteRegister(builder, layout, Gpr(0, bits), quotient);
    WriteRegister(builder, layout, bits == 8 ? _Operand{O_REG, R_AH, 8} : Gpr(2, bits), remainder);
}

template<OpCode op>
Value Binary(Emitter& builder, Value left, Operand right, Type type = Type::U64) {
    return Require(builder.Emit<op>(type, {Operand::ValueOf(left), right}));
}
Value Bit(Emitter& builder, Value value, unsigned bit) {
    return Require(builder.Emit<OpCode::TestBit>(Type::Void,
            {Operand::ValueOf(value), Operand::Immediate(bit, Type::U8)}));
}
void BitTest(Emitter& builder, StateLayout layout, const _DInst& instruction) {
    const auto& source = instruction.ops[0]; const auto& index = instruction.ops[1];
    if (source.type != O_REG || (source.size != 16 && source.size != 32 && source.size != 64) ||
        instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE ||
        (index.type != O_IMM && !(index.type == O_REG && index.size == source.size))) Unsupported();
    const auto value = ReadRegister(builder, layout, source.index, source.size);
    Value carry;
    if (index.type == O_IMM) carry = Bit(builder, value, instruction.imm.byte & (source.size - 1));
    else {
        const auto shift = ReadRegister(builder, layout, index.index, index.size);
        const auto selected = Binary<OpCode::LsrValue>(builder, value, Operand::ValueOf(shift), ScalarType(source.size));
        carry = Bit(builder, selected, 0);
    }
    Require(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(carry)}));
}
void Shift(Emitter& builder, StateLayout layout, const _DInst& instruction,
             PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0]; const auto& amount = instruction.ops[1];
    const auto bits = destination.size;
    ScalarType(bits);
    if ((destination.type != O_REG && !Memory(destination)) ||
        instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE ||
        (amount.type != O_IMM && !(amount.type == O_REG && amount.index == R_CL && amount.size == 8))) Unsupported();
    const bool variable = amount.type == O_REG;
    const unsigned constant = static_cast<unsigned>(instruction.imm.byte) & (bits == 64 ? 63 : 31);
    const bool rotate = instruction.opcode == I_ROL || instruction.opcode == I_ROR;
    const bool left = instruction.opcode == I_SHL || instruction.opcode == I_SAL || instruction.opcode == I_ROL;
    const bool arithmetic = instruction.opcode == I_SAR;
    Value count;
    if (variable) {
        count = ReadRegister(builder, layout, R_CL, 8);
        count = Binary<OpCode::And>(builder, count, Operand::Immediate(bits == 64 ? 63 : 31, Type::U8));
    }
    Value address;
    if (Memory(destination)) {
        PrepareFaultPC(builder, layout, pc, target);
        address = Address(builder, layout, instruction, destination, next_pc, target);
    }
    const auto original = address ? Require(builder.Emit<OpCode::LoadMemoryTSO>(ScalarType(bits), {Operand::ValueOf(address)}))
            : ReadRegister(builder, layout, destination.index, bits);
    Value input = original;
    const auto work_type = bits == 64 ? Type::U64 : Type::U32;
    if (bits < 32) input = arithmetic
            ? Require(builder.Emit<OpCode::SignExtend>(Type::U32, {Operand::ValueOf(original)}))
            : Require(builder.Emit<OpCode::ZeroExtend32>(Type::Void, {Operand::ValueOf(original)}));
    Value result;
    if (rotate) {
        if (bits < 32) {
            // Repeat the narrow bit pattern to fill the numeric 32-bit rotate.
            const auto doubled = Binary<OpCode::LslImm>(builder, input, Operand::Immediate(bits, Type::U8), Type::U32);
            input = Binary<OpCode::Or>(builder, input, Operand::ValueOf(doubled), Type::U32);
            if (bits == 8) {
                const auto upper = Binary<OpCode::LslImm>(builder, input, Operand::Immediate(16, Type::U8), Type::U32);
                input = Binary<OpCode::Or>(builder, input, Operand::ValueOf(upper), Type::U32);
            }
        }
        if (variable) {
            auto rotation = count;
            if (left) rotation = Binary<OpCode::Sub>(builder, Constant(builder, 0), Operand::ValueOf(count));
            result = Binary<OpCode::RorValue>(builder, input, Operand::ValueOf(rotation), work_type);
        } else {
            const auto rotation = (left ? 0u - constant : constant) & (bits == 64 ? 63u : 31u);
            result = Binary<OpCode::RorImm>(builder, input, Operand::Immediate(rotation, Type::U8), work_type);
        }
    } else if (variable) {
        if (left) result = Binary<OpCode::LslValue>(builder, input, Operand::ValueOf(count), work_type);
        else if (arithmetic) result = Binary<OpCode::AsrValue>(builder, input, Operand::ValueOf(count), work_type);
        else result = Binary<OpCode::LsrValue>(builder, input, Operand::ValueOf(count), work_type);
    } else {
        const auto immediate = Operand::Immediate(constant, Type::U8);
        if (left) result = Binary<OpCode::LslImm>(builder, input, immediate, work_type);
        else if (arithmetic) result = Binary<OpCode::AsrImm>(builder, input, immediate, work_type);
        else result = Binary<OpCode::LsrImm>(builder, input, immediate, work_type);
    }
    if (address) StoreMemory(builder, address, result, bits);
    else WriteRegister(builder, layout, destination, result);
    if (!variable && !constant) return; // Count zero preserves every flag.

    // The incoming flags were flushed before this instruction. Until a real
    // observer, no flags writer runs without consuming or replacing this one
    // recipe. Captured SSA inputs survive through their eventual actual uses;
    // a full writer can discard the recipe without birthing its flag chain.
    pending.recipe = {original, count, result, {}, bits, constant, instruction.opcode, false};
}

void PendingFlags::PublishShift(Emitter& builder) {
    const auto& [original, count, result, high, bits, constant, opcode, wide_product] = recipe;
    const bool variable = bool(count);
    const bool rotate = opcode == I_ROL || opcode == I_ROR;
    const bool left = opcode == I_SHL || opcode == I_SAL || opcode == I_ROL;
    const bool arithmetic = opcode == I_SAR;
    Value old_flags;
    if (variable) old_flags = Require(builder.Emit<OpCode::GetFlags>(Type::U64,
            {Operand::ValueOf(count), Operand::Flags(Flag::All)}));

    // All new flags are produced after a possible memory store has succeeded.
    // AF and undefined OF values are preserved; no packed backend layout leaks
    // into the frontend. Narrow shifts use x86's five-bit count, not modulo N.
    if (!rotate) {
        const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        Require(builder.Emit<OpCode::And>(ScalarType(bits),
                {Operand::ValueOf(result, ScalarType(bits)), Operand::Immediate(mask, ScalarType(bits))},
                {FlagBits(Flag::Negate | Flag::Zero | Flag::Parity), FlagProfile::LogicalZeroCVA, false}));
    }
    Value carry;
    if (rotate) carry = Bit(builder, result, left ? 0 : bits - 1);
    else if (!variable) {
        if (constant <= bits) carry = Bit(builder, original, left ? bits - constant : constant - 1);
        else carry = arithmetic ? Bit(builder, original, bits - 1) : Constant(builder, 0);
    }
    else {
        auto wide = Widen(builder, original, bits, arithmetic);
        auto index = left ? Binary<OpCode::Sub>(builder, Constant(builder, bits), Operand::ValueOf(count))
                : Binary<OpCode::Sub>(builder, count, Operand::Immediate(1, Type::U8));
        wide = Binary<OpCode::LsrValue>(builder, wide, Operand::ValueOf(index));
        carry = Bit(builder, wide, 0);
    }
    Require(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(carry)}));
    if (variable || constant == 1) {
        Value overflow;
        if (arithmetic) overflow = Constant(builder, 0);
        else if (!left && !rotate) overflow = Bit(builder, original, bits - 1);
        else overflow = Binary<OpCode::Xor>(builder, Bit(builder, result, bits - 1),
                Operand::ValueOf(left ? carry : Bit(builder, result, bits - 2)));
        if (variable) {
            const auto old_overflow = Require(builder.Emit<OpCode::TestFlags>(Type::Void, {Operand::Flags(Flag::Overflow)}));
            const auto different = Binary<OpCode::Xor>(builder, count, Operand::Immediate(1, Type::U8));
            const auto one = Require(builder.Emit<OpCode::TestZero>(Type::Void, {Operand::ValueOf(different)}));
            overflow = Require(builder.Emit<OpCode::Select>(Type::U64,
                    {Operand::ValueOf(one), Operand::ValueOf(overflow), Operand::ValueOf(old_overflow)}));
        }
        Require(builder.Emit<OpCode::SetOverflow>(Type::Void, {Operand::ValueOf(overflow)}));
    }
    if (variable) {
        const auto new_flags = Require(builder.Emit<OpCode::GetFlags>(Type::U64,
                {Operand::ValueOf(result), Operand::Flags(Flag::All)}));
        const auto chosen = Require(builder.Emit<OpCode::Select>(Type::U64,
                {Operand::ValueOf(count), Operand::ValueOf(new_flags), Operand::ValueOf(old_flags)}));
        Require(builder.Emit<OpCode::RestoreFlags>(Type::Void, {Operand::ValueOf(chosen), Operand::Flags(Flag::All)}));
    }
}
template<OpCode op, bool step = false>
void Alu(Emitter& builder, StateLayout layout, const _DInst& instruction,
         PendingFlags& pending, u64 pc, u64 next_pc, TargetInfo target, bool write_result = true) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    const auto bits = destination.size;
    const auto type = ScalarType(bits);
    if ((destination.type != O_REG && !Memory(destination)) ||
        instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE) Unsupported();
    if constexpr (op == OpCode::Neg || step) {
        if (source.type != O_NONE) Unsupported();
    } else {
        if ((source.type != O_REG && source.type != O_IMM && !Memory(source)) ||
            (source.type != O_IMM && source.size != bits) ||
            (Memory(destination) && Memory(source))) Unsupported();
    }
    if (Memory(destination) || Memory(source)) {
        pending.Flush(builder);
        PrepareFaultPC(builder, layout, pc, target);
    }
    // These writers consume/preserve the incoming carry, then replace every
    // other defined arithmetic bit. Unobserved old non-carry bits die here.
    if constexpr (step || op == OpCode::Adc || op == OpCode::Sbb)
        pending.Observe(builder, FlagBits(Flag::Carry));
    const bool locked = instruction.flags & FLAG_LOCK;
    Value address, left, incoming_carry;
    Operand right;
    if (locked) {
        if (!Memory(destination) || !write_result) Unsupported();
        address = Address(builder, layout, instruction, destination, next_pc, target);
        Value operand;
        if constexpr (op == OpCode::Neg) operand = Constant(builder, 0, type);
        else if constexpr (step) operand = Constant(builder, 1, type);
        else if (source.type == O_IMM) operand = Constant(builder, ImmediateBits(instruction, bits), type);
        else operand = ReadRegister(builder, layout, source.index, bits);
        if constexpr (op != OpCode::Neg) right = Operand::ValueOf(operand, type);
        Value carry_input;
        if constexpr (op == OpCode::Adc || op == OpCode::Sbb) {
            incoming_carry = Require(builder.Emit<OpCode::TestFlags>(Type::Void, {Operand::Flags(Flag::Carry)}));
            carry_input = incoming_carry;
        } else carry_input = Constant(builder, 0, Type::U8);
        constexpr auto kind = [] {
            if constexpr (op == OpCode::Add) return AtomicRMWOp::Add;
            else if constexpr (op == OpCode::Sub) return AtomicRMWOp::Sub;
            else if constexpr (op == OpCode::And) return AtomicRMWOp::And;
            else if constexpr (op == OpCode::Or) return AtomicRMWOp::Or;
            else if constexpr (op == OpCode::Xor) return AtomicRMWOp::Xor;
            else if constexpr (op == OpCode::Adc) return AtomicRMWOp::AddCarry;
            else if constexpr (op == OpCode::Sbb) return AtomicRMWOp::SubBorrow;
            else return AtomicRMWOp::Neg;
        }();
        left = Require(builder.Emit<OpCode::AtomicRMW>(type,
                {Operand::Immediate(static_cast<u64>(kind), Type::U8), Operand::ValueOf(address),
                 Operand::ValueOf(operand, type), Operand::ValueOf(carry_input)}));
        if (incoming_carry)
            Require(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(incoming_carry)}));
    } else {
        left = destination.type == O_REG ? ReadRegister(builder, layout, destination.index, bits) : [&] {
            address = Address(builder, layout, instruction, destination, next_pc, target);
            return Require(builder.Emit<OpCode::LoadMemoryTSO>(type, {Operand::ValueOf(address)}));
        }();
        if constexpr (op != OpCode::Neg) {
            if constexpr (step) right = Operand::Immediate(1, type);
            else if (source.type == O_IMM) right = Operand::Immediate(ImmediateBits(instruction, bits), type);
            else if (destination.type == O_REG && source.type == O_REG &&
                     destination.index == source.index && destination.size == source.size)
                right = Operand::ValueOf(left);
            else right = Operand::ValueOf(ReadData(builder, layout, instruction, source, next_pc, target));
        }
        if constexpr (op == OpCode::Adc || op == OpCode::Sbb)
            if (write_result && Memory(destination))
                incoming_carry = Require(builder.Emit<OpCode::TestFlags>(Type::Void, {Operand::Flags(Flag::Carry)}));
    }
    const auto calculate = [&] {
        if constexpr (op == OpCode::Neg)
            return Require(builder.Emit<op>(type, {Operand::ValueOf(left)}));
        else return Require(builder.Emit<op>(type, {Operand::ValueOf(left), right}));
    };
    const auto result = calculate();
    Value flags = result;
    if (write_result && !locked) {
        if (destination.type == O_REG) WriteRegister(builder, layout, destination, result);
        else {
            StoreMemory(builder, address, result, bits);
            if (incoming_carry)
                Require(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(incoming_carry)}));
            // The store may fault. Keep old flags until it succeeds, then
            // produce the new flags from the same real operand edges. A later
            // full writer can discard this otherwise unused second result.
            flags = calculate();
        }
    }
    // Every accepted ALU here replaces all arithmetic flags. With no observer
    // since the preceding writer, only the latest producer needs publication.
    pending = {flags.node, op == OpCode::And || op == OpCode::Or || op == OpCode::Xor
            ? FlagProfile::LogicalZeroCVA : FlagProfile::ArithmeticCanonicalCarry,
            step ? FlagBits(Flag::All) & ~FlagBits(Flag::Carry) : FlagBits(Flag::All), {}};
}

bool VRegister(const _Operand& operand) {
    return operand.type == O_REG && operand.index >= R_XMM0 && operand.index <= R_XMM15;
}
u32 VOffset(StateLayout layout, const _Operand& operand) {
    if (!VRegister(operand) || layout.fprs_offset == UINT32_MAX ||
        layout.fprs_offset > UINT32_MAX - 255) Unsupported();
    return layout.fprs_offset + (operand.index - R_XMM0) * 16;
}
bool AlignedLiteralAddress(const _DInst& instruction, const _Operand& operand, u64 next_pc) {
    const auto segment = SEGMENT_GET(instruction.segment);
    if (segment == R_FS || segment == R_GS) return false;
    if (operand.type == O_DISP) return !(instruction.disp & 15);
    return operand.type == O_SMEM && operand.index == R_RIP && !((next_pc + instruction.disp) & 15);
}
Value VRead(Emitter& builder, StateLayout layout, const _DInst& instruction,
            const _Operand& operand, u64 pc, u64 next_pc, TargetInfo target, bool aligned = false) {
    if (VRegister(operand)) {
        const auto offset = VOffset(layout, operand);
        const auto slot = operand.index - R_XMM0;
        const auto bit = static_cast<std::uint16_t>(1u << slot);
        if (builder.private_vectors && (builder.valid_vectors & bit))
            return {builder.UnitId(), builder.vectors[slot]};
        const auto value = Require(builder.Emit<OpCode::LoadUniform>(Type::V128,
                {Operand::Uniform(offset, Type::V128)}));
        if (builder.private_vectors) { builder.vectors[slot] = value.node; builder.valid_vectors |= bit; }
        return value;
    }
    if (!Memory(operand) || operand.size != 128) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    u64 displacement{};
    const auto address = Address(builder, layout, instruction, operand, next_pc, target, {},
                                 aligned ? nullptr : &displacement);
    if (aligned && !AlignedLiteralAddress(instruction, operand, next_pc))
        Require(builder.Emit<OpCode::CheckMemoryAlignment>(Type::Void,
                {Operand::ValueOf(address), Operand::Immediate(15, Type::U8)}));
    return LoadMemory(builder, Type::V128, address, displacement);
}
template<OpCode op>
void VWrite(Emitter& builder, StateLayout layout, const _DInst& instruction,
            const _Operand& operand, Value value, u64 pc, u64 next_pc, TargetInfo target, bool aligned) {
    if (VRegister(operand)) {
        const auto offset = VOffset(layout, operand);
        if (builder.private_vectors) {
            const auto slot = operand.index - R_XMM0;
            const auto bit = static_cast<std::uint16_t>(1u << slot);
            builder.vectors[slot] = value.node;
            builder.valid_vectors |= bit;
            builder.dirty_vectors |= bit;
            return;
        }
        Require(builder.Emit<OpCode::StoreUniform>(Type::Void,
                {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value, Type::V128)}));
        return;
    }
    if (!Memory(operand) || operand.size != 128) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    u64 displacement{};
    const auto address = Address(builder, layout, instruction, operand, next_pc, target, {},
                                 aligned ? nullptr : &displacement);
    if (aligned && !AlignedLiteralAddress(instruction, operand, next_pc))
        Require(builder.Emit<OpCode::CheckMemoryAlignment>(Type::Void,
                {Operand::ValueOf(address), Operand::Immediate(15, Type::U8)}));
    StoreMemory<op>(builder, address, value, 128, displacement);
}
Value VScalarRead(Emitter& builder, StateLayout layout, const _DInst& instruction,
                  const _Operand& operand, unsigned bits, u64 pc, u64 next_pc, TargetInfo target) {
    if (VRegister(operand)) {
        if (bits != 64) Unsupported();
        const auto source = VRead(builder, layout, instruction, operand, pc, next_pc, target);
        return Require(builder.Emit<OpCode::VecExtract64>(Type::U64,
                {Operand::ValueOf(source), Operand::Immediate(0, Type::U8)}));
    }
    if (operand.size != bits) Unsupported();
    if (operand.type == O_REG) return ReadRegister(builder, layout, operand.index, bits);
    if (!Memory(operand)) Unsupported();
    PrepareFaultPC(builder, layout, pc, target);
    return ReadData(builder, layout, instruction, operand, next_pc, target);
}
Value VLowScalar(Emitter& builder, Value scalar, unsigned bits) {
    if (bits == 32)
        scalar = Require(builder.Emit<OpCode::ZeroExtend64>(Type::U64,
                {Operand::ValueOf(scalar, Type::U32)}));
    const auto duplicate = Require(builder.Emit<OpCode::VecDup64>(Type::V128, {Operand::ValueOf(scalar)}));
    const auto zero = Require(builder.Emit<OpCode::VecSharedZero>(Type::V128, {}));
    // Explicitly clear high64, without changing raw BitCast semantics.
    return Require(builder.Emit<OpCode::VecByteShift>(Type::V128,
            {Operand::ValueOf(duplicate), Operand::ValueOf(zero),
             Operand::Immediate(8, Type::U8), Operand::Immediate(0, Type::U8)}));
}
void ScalarDouble(Emitter& builder, StateLayout layout, const _DInst& instruction,
                  std::span<const std::uint8_t> bytes, PendingFlags& pending,
                  u64 pc, u64 next_pc, TargetInfo target) {
    if (instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE) Unsupported();
    auto destination = instruction.ops[0], source = instruction.ops[1];
    const auto preserve_high = [&](Value low, Value previous) {
        return Require(builder.Emit<OpCode::VecShuffle32TwoSrc>(Type::V128,
                {Operand::ValueOf(low), Operand::ValueOf(previous), Operand::Immediate(0xe4, Type::U8)}));
    };
    const auto scalar_vector = [&](const _Operand& operand) {
        if (VRegister(operand)) return VRead(builder, layout, instruction, operand, pc, next_pc, target);
        if (!Memory(operand) || operand.size != 64) Unsupported();
        const auto low = VScalarRead(builder, layout, instruction, operand, 64, pc, next_pc, target);
        // Arithmetic/compare observe only low64; do not clear unused lanes.
        return Require(builder.Emit<OpCode::VecDup64>(Type::V128, {Operand::ValueOf(low)}));
    };
    if (instruction.opcode == I_MOVSD) {
        if (VRegister(destination)) {
            if (!VRegister(source) && (!Memory(source) || source.size != 64)) Unsupported();
            Value result;
            if (VRegister(source)) {
                if (source.index == destination.index) return;
                const auto low = VRead(builder, layout, instruction, source, pc, next_pc, target);
                const auto previous = VRead(builder, layout, instruction, destination, pc, next_pc, target);
                result = preserve_high(low, previous);
            } else {
                pending.Flush(builder);
                result = VLowScalar(builder,
                        VScalarRead(builder, layout, instruction, source, 64, pc, next_pc, target), 64);
            }
            VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        } else {
            if (!Memory(destination) || destination.size != 64 || !VRegister(source)) Unsupported();
            pending.Flush(builder);
            PrepareFaultPC(builder, layout, pc, target);
            const auto low = VScalarRead(builder, layout, instruction, source, 64, pc, next_pc, target);
            u64 displacement{};
            const auto address = Address(builder, layout, instruction, destination, next_pc, target, {}, &displacement);
            StoreMemory(builder, address, low, 64, displacement);
        }
        return;
    }
    if (instruction.opcode == I_CVTSI2SD || instruction.opcode == I_CVTTSD2SI) {
        // These conversions have only 32/64-bit integer forms. Operand-size
        // override never selects a word conversion; effective REX.W does.
        std::uint8_t rex{};
        for (const auto byte : bytes) {
            if (byte == 0x0f) break;
            rex = byte >= 0x40 && byte <= 0x4f ? byte : 0;
        }
        const unsigned bits = rex & 8 ? 64 : 32;
        const auto type = ScalarType(bits);
        if (instruction.opcode == I_CVTSI2SD) {
            if (!VRegister(destination) || (source.type != O_REG && !Memory(source)) ||
                (source.size != 16 && source.size != 32 && source.size != 64)) Unsupported();
            if (source.type == O_REG) {
                unsigned slot{};
                RegisterOffset(layout, source.index, source.size, slot);
                source = Gpr(slot, bits);
            } else source.size = static_cast<std::uint16_t>(bits);
            if (Memory(source)) { pending.Flush(builder); PrepareFaultPC(builder, layout, pc, target); }
            const auto input = ReadData(builder, layout, instruction, source, next_pc, target);
            const auto previous = VRead(builder, layout, instruction, destination, pc, next_pc, target);
            const auto converted = Require(builder.Emit<OpCode::VecFCvtIntToFloat>(Type::U64,
                    {Operand::ValueOf(input, type), Operand::Immediate(bits, Type::U8), Operand::Immediate(64, Type::U8)}));
            const auto low = Require(builder.Emit<OpCode::VecDup64>(Type::V128, {Operand::ValueOf(converted)}));
            VWrite(builder, layout, instruction, destination, preserve_high(low, previous), pc, next_pc, target);
        } else {
            if (destination.type != O_REG || (destination.size != 16 && destination.size != 32 && destination.size != 64) ||
                (!VRegister(source) && (!Memory(source) || source.size != 64))) Unsupported();
            unsigned slot{};
            RegisterOffset(layout, destination.index, destination.size, slot);
            if (Memory(source)) pending.Flush(builder);
            const auto input = VScalarRead(builder, layout, instruction, source, 64, pc, next_pc, target);
            const auto result = Require(builder.Emit<OpCode::VecFCvtFloatToInt>(type,
                    {Operand::ValueOf(input, Type::U64), Operand::Immediate(64, Type::U8),
                     Operand::Immediate(bits, Type::U8), Operand::Immediate(1, Type::U8)}));
            WriteRegister(builder, layout, Gpr(slot, bits), result);
        }
        return;
    }
    if (!VRegister(destination) || (!VRegister(source) && (!Memory(source) || source.size != 64))) Unsupported();
    if (Memory(source)) pending.Flush(builder);
    const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
    const auto right = scalar_vector(source);
    if (instruction.opcode == I_ADDSD || instruction.opcode == I_SUBSD ||
        instruction.opcode == I_MULSD || instruction.opcode == I_DIVSD) {
        Value result;
        switch (instruction.opcode) {
            case I_ADDSD:
                result = Require(builder.Emit<OpCode::VecFAddScalar64>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right)})); break;
            case I_SUBSD:
                result = Require(builder.Emit<OpCode::VecFSubScalar64>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right)})); break;
            case I_MULSD:
                result = Require(builder.Emit<OpCode::VecFMulScalar64>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right)})); break;
            default:
                result = Require(builder.Emit<OpCode::VecFDivScalar64>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right)})); break;
        }
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
    } else {
        const auto compared = Require(builder.Emit<OpCode::VecFCmp>(Type::U32,
                {Operand::ValueOf(left), Operand::ValueOf(right), Operand::Immediate(64, Type::U8)}));
        Require(builder.Emit<OpCode::PublishFCmpFlags>(Type::Void, {Operand::ValueOf(compared)}));
        pending = {}; // The successful compare replaces all arithmetic flags.
    }
}
bool Vector(Emitter& builder, StateLayout layout, const _DInst& instruction,
            u64 pc, u64 next_pc, TargetInfo target) {
    unsigned source_bits{}, destination_bits{}, shift_bits{}, relation{UINT32_MAX};
    bool sign{}, shift_left{}, shift_arithmetic{};
    switch (instruction.opcode) {
        case I_MOVUPS: case I_MOVAPS: case I_MOVUPD: case I_MOVAPD: case I_MOVDQU: case I_MOVDQA:
        case I_MOVD: case I_MOVQ: case I_MOVLPD: case I_MOVHPD:
        case I_PXOR: case I_XORPS: case I_XORPD:
        case I_PAND: case I_ANDPD: case I_ANDPS:
        case I_PANDN: case I_ANDNPD: case I_ANDNPS:
        case I_POR: case I_ORPD: case I_ORPS:
        case I_CVTTPD2DQ: case I_CVTDQ2PD:
        case I_PADDB: case I_PADDW: case I_PADDD: case I_PADDQ:
        case I_PSUBB: case I_PSUBW: case I_PSUBD: case I_PSUBQ:
        case I_PSHUFD: case I_PSHUFLW: case I_SHUFPS:
        case I_PALIGNR: case I_PSLLDQ: case I_PSRLDQ:
        case I_PUNPCKLBW: case I_PUNPCKLWD: case I_PUNPCKLDQ:
        case I_PMULUDQ: case I_PMULLW: case I_PMULLD: case I_PMADDWD:
        case I_PCMPEQB: case I_PCMPEQW: case I_PCMPEQD:
        case I_PCMPGTB: case I_PCMPGTW: case I_PCMPGTD:
        case I_PMOVMSKB: case I_MOVMSKPS: case I_MOVMSKPD: break;
        // Distorm consumed the hidden imm8 and selected these legacy aliases.
        // The neutral relation bits are less/equal/greater/unordered, not the
        // x86 comparison encoding or a host condition code.
        case I_CMPEQPD: relation = 2; break;
        case I_CMPLTPD: relation = 1; break;
        case I_CMPLEPD: relation = 3; break;
        case I_CMPUNORDPD: relation = 8; break;
        case I_CMPNEQPD: relation = 13; break;
        case I_CMPNLTPD: relation = 14; break;
        case I_CMPNLEPD: relation = 12; break;
        case I_CMPORDPD: relation = 7; break;
        case I_PSLLW: shift_left = true; [[fallthrough]];
        case I_PSRLW: shift_bits = 16; break;
        case I_PSLLD: shift_left = true; [[fallthrough]];
        case I_PSRLD: shift_bits = 32; break;
        case I_PSLLQ: shift_left = true; [[fallthrough]];
        case I_PSRLQ: shift_bits = 64; break;
        case I_PSRAD: shift_bits = 32; shift_arithmetic = true; break;
        case I_PMOVSXBW: sign = true; [[fallthrough]];
        case I_PMOVZXBW: source_bits = 8; destination_bits = 16; break;
        case I_PMOVSXBD: sign = true; [[fallthrough]];
        case I_PMOVZXBD: source_bits = 8; destination_bits = 32; break;
        case I_PMOVSXBQ: sign = true; [[fallthrough]];
        case I_PMOVZXBQ: source_bits = 8; destination_bits = 64; break;
        case I_PMOVSXWD: sign = true; [[fallthrough]];
        case I_PMOVZXWD: source_bits = 16; destination_bits = 32; break;
        case I_PMOVSXWQ: sign = true; [[fallthrough]];
        case I_PMOVZXWQ: source_bits = 16; destination_bits = 64; break;
        case I_PMOVSXDQ: sign = true; [[fallthrough]];
        case I_PMOVZXDQ: source_bits = 32; destination_bits = 64; break;
        default: return false;
    }
    if (layout.fprs_offset == UINT32_MAX || instruction.ops[3].type != O_NONE) Unsupported();
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    if (instruction.opcode == I_MOVLPD || instruction.opcode == I_MOVHPD) {
        if (instruction.ops[2].type != O_NONE) Unsupported();
        const bool load = VRegister(destination);
        const auto& reg = load ? destination : source;
        const auto& memory = load ? source : destination;
        if (!VRegister(reg) || !Memory(memory) || memory.size != 64) Unsupported();
        const auto offset = VOffset(layout, reg) + (instruction.opcode == I_MOVHPD ? 8 : 0);
        if (load) {
            PrepareFaultPC(builder, layout, pc, target);
            const auto value = ReadData(builder, layout, instruction, memory, next_pc, target);
            Require(builder.Emit<OpCode::StoreUniform>(Type::Void,
                    {Operand::Uniform(offset, Type::U64), Operand::ValueOf(value)}));
            const auto bit = static_cast<std::uint16_t>(1u << (reg.index - R_XMM0));
            builder.valid_vectors = static_cast<std::uint16_t>(builder.valid_vectors & ~bit);
            builder.dirty_vectors = static_cast<std::uint16_t>(builder.dirty_vectors & ~bit);
        } else {
            PrepareFaultPC(builder, layout, pc, target);
            const auto value = Require(builder.Emit<OpCode::LoadUniform>(Type::U64,
                    {Operand::Uniform(offset, Type::U64)}));
            u64 displacement{};
            const auto address = Address(builder, layout, instruction, memory, next_pc, target, {}, &displacement);
            StoreMemory(builder, address, value, 64, displacement);
        }
        return true;
    }
    if (source_bits) {
        if (!VRegister(destination) || destination.size != 128 || instruction.ops[2].type != O_NONE) Unsupported();
        Value value;
        if (VRegister(source)) value = VRead(builder, layout, instruction, source, pc, next_pc, target);
        else {
            if (!Memory(source)) Unsupported();
            auto narrow = source;
            narrow.size = static_cast<std::uint16_t>(128 / destination_bits * source_bits);
            PrepareFaultPC(builder, layout, pc, target);
            const auto scalar = ReadData(builder, layout, instruction, narrow, next_pc, target);
            // VecExtend observes only the low 2/4/8 bytes, so the duplicated
            // upper half needs no additional clear or full-width memory read.
            value = Require(builder.Emit<OpCode::VecDup64>(Type::V128, {Operand::ValueOf(scalar)}));
        }
        const auto result = Require(builder.Emit<OpCode::VecExtend>(Type::V128,
                {Operand::ValueOf(value), Operand::Immediate(source_bits, Type::U8),
                 Operand::Immediate(destination_bits, Type::U8), Operand::Immediate(sign, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == I_CVTTPD2DQ || instruction.opcode == I_CVTDQ2PD) {
        if (!VRegister(destination) || destination.size != 128 || instruction.ops[2].type != O_NONE) Unsupported();
        Value input;
        if (VRegister(source)) {
            input = VRead(builder, layout, instruction, source, pc, next_pc, target);
        } else if (instruction.opcode == I_CVTDQ2PD) {
            if (!Memory(source) || source.size != 64) Unsupported();
            // Only two signed dwords participate. Load their actual eight
            // bytes; the duplicate's other half is not observed by kind 1.
            const auto low = VScalarRead(builder, layout, instruction, source, 64, pc, next_pc, target);
            input = Require(builder.Emit<OpCode::VecDup64>(Type::V128, {Operand::ValueOf(low)}));
        } else {
            if (!Memory(source) || source.size != 128) Unsupported();
            input = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        }
        const auto result = Require(builder.Emit<OpCode::VecFCvtPacked>(Type::V128,
                {Operand::ValueOf(input), Operand::Immediate(instruction.opcode == I_CVTTPD2DQ ? 5 : 1, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (relation != UINT32_MAX) {
        if (!VRegister(destination) || destination.size != 128 || source.size != 128 ||
            instruction.ops[2].type != O_NONE) Unsupported();
        const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        const auto right = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        const auto result = Require(builder.Emit<OpCode::VecFCmpMask>(Type::V128,
                {Operand::ValueOf(left), Operand::ValueOf(right), Operand::Immediate(64, Type::U8),
                 Operand::Immediate(relation, Type::U8), Operand::Immediate(0, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == I_PMOVMSKB || instruction.opcode == I_MOVMSKPS || instruction.opcode == I_MOVMSKPD) {
        if (instruction.ops[2].type != O_NONE || destination.type != O_REG || destination.size != 32 ||
            !VRegister(source) || source.size != 128) Unsupported();
        const auto value = VRead(builder, layout, instruction, source, pc, next_pc, target);
        const unsigned lane = instruction.opcode == I_PMOVMSKB ? 8 : instruction.opcode == I_MOVMSKPS ? 32 : 64;
        const auto mask = Require(builder.Emit<OpCode::VecMovMask>(Type::U32,
                {Operand::ValueOf(value), Operand::Immediate(lane, Type::U8)}));
        WriteRegister(builder, layout, destination, mask);
        return true;
    }
    if (instruction.opcode == I_MOVD || instruction.opcode == I_MOVQ) {
        if (instruction.ops[2].type != O_NONE || (!VRegister(destination) && !VRegister(source))) Unsupported();
        const unsigned bits = instruction.opcode == I_MOVQ || destination.size == 64 || source.size == 64 ? 64 : 32;
        if (VRegister(destination)) {
            const auto scalar = VScalarRead(builder, layout, instruction, source, bits, pc, next_pc, target);
            VWrite(builder, layout, instruction, destination, VLowScalar(builder, scalar, bits), pc, next_pc, target);
        } else {
            const auto vector = VRead(builder, layout, instruction, source, pc, next_pc, target);
            const auto value = Require(builder.Emit<OpCode::VecExtract64>(Type::U64,
                    {Operand::ValueOf(vector), Operand::Immediate(0, Type::U8)}));
            if (destination.size != bits) Unsupported();
            if (destination.type == O_REG) WriteRegister(builder, layout, destination, value);
            else {
                if (!Memory(destination)) Unsupported();
                PrepareFaultPC(builder, layout, pc, target);
                u64 displacement{};
                const auto address = Address(builder, layout, instruction, destination, next_pc, target, {}, &displacement);
                StoreMemory(builder, address, value, bits, displacement);
            }
        }
        return true;
    }
    if (instruction.opcode == I_MOVUPS || instruction.opcode == I_MOVAPS ||
        instruction.opcode == I_MOVUPD || instruction.opcode == I_MOVAPD ||
        instruction.opcode == I_MOVDQU || instruction.opcode == I_MOVDQA) {
        if (instruction.ops[2].type != O_NONE || destination.size != 128 || source.size != 128 ||
            (!VRegister(destination) && !VRegister(source))) Unsupported();
        const bool aligned = instruction.opcode == I_MOVAPS || instruction.opcode == I_MOVAPD ||
                instruction.opcode == I_MOVDQA;
        const auto value = VRead(builder, layout, instruction, source, pc, next_pc, target, aligned);
        VWrite(builder, layout, instruction, destination, value, pc, next_pc, target, aligned);
        return true;
    }
    if (!VRegister(destination) || destination.size != 128) Unsupported();
    if (instruction.opcode == I_PALIGNR) {
        if (instruction.ops[2].type != O_IMM || instruction.ops[2].size != 8 || source.size != 128 ||
            (!VRegister(source) && !Memory(source))) Unsupported();
        const auto count = instruction.imm.byte;
        Value low;
        // A memory source is still accessed when the count discards all its
        // bytes. Pure register sources need no such unused read.
        if (count < 16 || Memory(source))
            low = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        Value result;
        if (count >= 32) result = Require(builder.Emit<OpCode::VecSharedZero>(Type::V128, {}));
        else if (!count) result = low;
        else {
            const auto high = VRead(builder, layout, instruction, destination, pc, next_pc, target);
            if (count < 16)
                result = Require(builder.Emit<OpCode::VecExtractBytes>(Type::V128,
                        {Operand::ValueOf(low), Operand::ValueOf(high), Operand::Immediate(count, Type::U8)}));
            else if (count == 16) result = high;
            else {
                const auto zero = Require(builder.Emit<OpCode::VecSharedZero>(Type::V128, {}));
                result = Require(builder.Emit<OpCode::VecByteShift>(Type::V128,
                        {Operand::ValueOf(high), Operand::ValueOf(zero),
                         Operand::Immediate(count - 16, Type::U8), Operand::Immediate(0, Type::U8)}));
            }
        }
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == I_PSLLDQ || instruction.opcode == I_PSRLDQ) {
        if (instruction.ops[2].type != O_NONE || source.type != O_IMM || source.size != 8) Unsupported();
        const auto count = instruction.imm.byte;
        if (!count) return true;
        const auto zero = Require(builder.Emit<OpCode::VecSharedZero>(Type::V128, {}));
        auto result = zero;
        if (count < 16) {
            const auto value = VRead(builder, layout, instruction, destination, pc, next_pc, target);
            result = Require(builder.Emit<OpCode::VecByteShift>(Type::V128,
                    {Operand::ValueOf(value), Operand::ValueOf(zero), Operand::Immediate(count, Type::U8),
                     Operand::Immediate(instruction.opcode == I_PSLLDQ, Type::U8)}));
        }
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == I_SHUFPS) {
        if (instruction.ops[2].type != O_IMM || instruction.ops[2].size != 8 || source.size != 128 ||
            (!VRegister(source) && !Memory(source))) Unsupported();
        const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        const auto right = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        const auto result = instruction.imm.byte == 0x88
                ? Require(builder.Emit<OpCode::VecUnzip>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right),
                         Operand::Immediate(32, Type::U8), Operand::Immediate(0, Type::U8)}))
                : Require(builder.Emit<OpCode::VecShuffle32TwoSrc>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right), Operand::Immediate(instruction.imm.byte, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.opcode == I_PSHUFD || instruction.opcode == I_PSHUFLW) {
        if (instruction.ops[2].type != O_IMM || instruction.ops[2].size != 8 || source.size != 128) Unsupported();
        const auto value = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        const auto control = Operand::Immediate(instruction.imm.byte, Type::U8);
        const auto result = instruction.opcode == I_PSHUFD
                ? Require(builder.Emit<OpCode::VecShuffle32>(Type::V128, {Operand::ValueOf(value), control}))
                : Require(builder.Emit<OpCode::VecShuffle16>(Type::V128,
                        {Operand::ValueOf(value), control, Operand::Immediate(0, Type::U8)}));
        VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
        return true;
    }
    if (instruction.ops[2].type != O_NONE) Unsupported();
    Value result;
    if (shift_bits) {
        if (source.type != O_IMM || source.size != 8) Unsupported();
        const auto count = instruction.imm.byte;
        if (!count) return true;
        if (!shift_arithmetic && count >= shift_bits)
            result = Require(builder.Emit<OpCode::VecSharedZero>(Type::V128, {}));
        else {
            const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
            const auto amount = Operand::Immediate(count, Type::U8), bits = Operand::Immediate(shift_bits, Type::U8);
            result = shift_left
                    ? Require(builder.Emit<OpCode::VecShiftLeftImm>(Type::V128, {Operand::ValueOf(left), amount, bits}))
                    : shift_arithmetic
                            ? Require(builder.Emit<OpCode::VecShiftRightArithmeticImm>(Type::V128, {Operand::ValueOf(left), amount, bits}))
                            : Require(builder.Emit<OpCode::VecShiftRightImm>(Type::V128, {Operand::ValueOf(left), amount, bits}));
        }
    } else {
        if (source.size != 128) Unsupported();
        const auto left = VRead(builder, layout, instruction, destination, pc, next_pc, target);
        // These legacy full-m128 computations require a 16-byte aligned EA.
        // VRead elides the check before birth for a proved literal address.
        const auto right = VRead(builder, layout, instruction, source, pc, next_pc, target, true);
        switch (instruction.opcode) {
            case I_PXOR: case I_XORPS: case I_XORPD:
                result = Require(builder.Emit<OpCode::VecXor>(Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)})); break;
            case I_PAND: case I_ANDPD: case I_ANDPS:
                result = Require(builder.Emit<OpCode::VecAnd>(Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)})); break;
            case I_PANDN: case I_ANDNPD: case I_ANDNPS:
                // VecAndNot(a,b) = a & ~b; the legacy destination is inverted.
                result = Require(builder.Emit<OpCode::VecAndNot>(Type::V128, {Operand::ValueOf(right), Operand::ValueOf(left)})); break;
            case I_POR: case I_ORPD: case I_ORPS:
                result = Require(builder.Emit<OpCode::VecOr>(Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)})); break;
            case I_PADDB: case I_PADDW: case I_PADDD: case I_PADDQ:
                result = Require(builder.Emit<OpCode::VecAdd>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == I_PADDB ? 8 : instruction.opcode == I_PADDW ? 16 :
                                 instruction.opcode == I_PADDD ? 32 : 64, Type::U8)})); break;
            case I_PSUBB: case I_PSUBW: case I_PSUBD: case I_PSUBQ:
                result = Require(builder.Emit<OpCode::VecSub>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == I_PSUBB ? 8 : instruction.opcode == I_PSUBW ? 16 :
                                 instruction.opcode == I_PSUBD ? 32 : 64, Type::U8)})); break;
            case I_PMULLW: case I_PMULLD:
                result = Require(builder.Emit<OpCode::VecMul>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == I_PMULLW ? 16 : 32, Type::U8)})); break;
            case I_PCMPEQB: case I_PCMPEQW: case I_PCMPEQD:
                result = Require(builder.Emit<OpCode::VecCmpEq>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == I_PCMPEQB ? 8 : instruction.opcode == I_PCMPEQW ? 16 : 32, Type::U8)})); break;
            case I_PCMPGTB: case I_PCMPGTW: case I_PCMPGTD:
                result = Require(builder.Emit<OpCode::VecCmpGt>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == I_PCMPGTB ? 8 : instruction.opcode == I_PCMPGTW ? 16 : 32, Type::U8)})); break;
            case I_PMULUDQ:
                result = Require(builder.Emit<OpCode::VecMulWiden>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right), Operand::Immediate(32, Type::U8), Operand::Immediate(0, Type::U8)})); break;
            case I_PMADDWD:
                result = Require(builder.Emit<OpCode::VecMadd16>(Type::V128, {Operand::ValueOf(left), Operand::ValueOf(right)})); break;
            case I_PUNPCKLBW: case I_PUNPCKLWD: case I_PUNPCKLDQ:
                result = Require(builder.Emit<OpCode::VecZip>(Type::V128,
                        {Operand::ValueOf(left), Operand::ValueOf(right),
                         Operand::Immediate(instruction.opcode == I_PUNPCKLBW ? 8 : instruction.opcode == I_PUNPCKLWD ? 16 : 32, Type::U8),
                         Operand::Immediate(0, Type::U8)})); break;
            default: Unsupported();
        }
    }
    VWrite(builder, layout, instruction, destination, result, pc, next_pc, target);
    return true;
}

bool DirectCondition(unsigned opcode, Condition& condition) {
    switch (opcode) {
        case I_JB: case I_SETB: case I_CMOVB: condition = Condition::CS; break;
        case I_JAE: case I_SETAE: case I_CMOVAE: condition = Condition::CC; break;
        case I_JO: case I_SETO: case I_CMOVO: condition = Condition::VS; break;
        case I_JNO: case I_SETNO: case I_CMOVNO: condition = Condition::VC; break;
        case I_JZ: case I_SETZ: case I_CMOVZ: condition = Condition::EQ; break;
        case I_JNZ: case I_SETNZ: case I_CMOVNZ: condition = Condition::NE; break;
        case I_JS: case I_SETS: case I_CMOVS: condition = Condition::MI; break;
        case I_JNS: case I_SETNS: case I_CMOVNS: condition = Condition::PL; break;
        case I_JL: case I_SETL: case I_CMOVL: condition = Condition::LT; break;
        case I_JGE: case I_SETGE: case I_CMOVGE: condition = Condition::GE; break;
        case I_JLE: case I_SETLE: case I_CMOVLE: condition = Condition::LE; break;
        case I_JG: case I_SETG: case I_CMOVG: condition = Condition::GT; break;
        default: return false;
    }
    return true;
}

Value BranchCondition(Emitter& builder, unsigned opcode) {
    Condition condition;
    if (DirectCondition(opcode, condition))
        return Require(builder.Emit<OpCode::CondSet>(Type::Void, {Operand::Condition(condition)}));
    const auto test = [&](Flag flags, bool set) {
        return set ? Require(builder.Emit<OpCode::TestFlags>(Type::Void, {Operand::Flags(flags)}))
                   : Require(builder.Emit<OpCode::TestNotFlags>(Type::Void, {Operand::Flags(flags)}));
    };
    switch (opcode) {
        case I_JBE: case I_SETBE: case I_CMOVBE: return test(Flag::Carry | Flag::Zero, true);
        case I_JA: case I_SETA: case I_CMOVA: return test(Flag::Carry | Flag::Zero, false);
        case I_JP: case I_SETP: case I_CMOVP: return test(Flag::Parity, true);
        case I_JNP: case I_SETNP: case I_CMOVNP: return test(Flag::Parity, false);
        default: Unsupported();
    }
}

void ConditionalSet(Emitter& builder, StateLayout layout, const _DInst& instruction,
                     u64 pc, u64 next_pc, TargetInfo target) {
    OneOperand(instruction);
    const auto& destination = instruction.ops[0];
    if ((destination.type != O_REG && !Memory(destination)) || destination.size != 8) Unsupported();
    if (Memory(destination)) PrepareFaultPC(builder, layout, pc, target);
    const auto condition = BranchCondition(builder, instruction.opcode);
    if (destination.type == O_REG) WriteRegister(builder, layout, destination, condition);
    else StoreMemory(builder, Address(builder, layout, instruction, destination, next_pc, target), condition, 8);
}
void ConditionalMove(Emitter& builder, StateLayout layout, const _DInst& instruction,
                      u64 pc, u64 next_pc, TargetInfo target) {
    const auto& destination = instruction.ops[0];
    const auto& source = instruction.ops[1];
    if (destination.type != O_REG || (destination.size != 16 && destination.size != 32 && destination.size != 64) ||
        (source.type != O_REG && !Memory(source)) || source.size != destination.size ||
        instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE) Unsupported();
    if (Memory(source)) PrepareFaultPC(builder, layout, pc, target);
    // CMOV's memory operand is read even when the condition is false.
    const auto value = ReadData(builder, layout, instruction, source, next_pc, target);
    const auto old = ReadRegister(builder, layout, destination.index, destination.size);
    Condition condition;
    Value selected;
    if (DirectCondition(instruction.opcode, condition))
        selected = Require(builder.Emit<OpCode::CondSelect>(ScalarType(destination.size),
                {Operand::Condition(condition), Operand::ValueOf(value), Operand::ValueOf(old)}));
    else {
        const auto check = BranchCondition(builder, instruction.opcode);
        selected = Require(builder.Emit<OpCode::Select>(ScalarType(destination.size),
                {Operand::ValueOf(check), Operand::ValueOf(value), Operand::ValueOf(old)}));
    }
    // A 32-bit CMOV zeroes the upper half on either path; 16-bit CMOV preserves it.
    WriteRegister(builder, layout, destination, selected);
}

// Byte decoding can stop after an already decoded prefix. Its error belongs at
// that frontier, after those actual instructions have gone through the same
// emitter; it must not hide an earlier unsupported opcode or manufacture a
// fallthrough/flags publication at the failed frontier.
struct DecodeFailure {
    std::size_t block{std::numeric_limits<std::size_t>::max()};
    BuildError error{BuildError::None};
    Error detail{Error::None};
};

BuildResult EmitDecoded(std::span<const _DInst> instructions, std::span<const DecodedBlock> blocks,
                        std::span<const BlockWindow> sources, Scope scope, StateLayout layout,
                        TargetInfo target, WorkCounts* collected, DecodeFailure failure = {}, u32 entry_block = 0,
                        bool external_edges = false) {
    if (collected) *collected = {};
    if (!ValidRequest(blocks.size(), scope, layout, target) || !ValidSources(sources) || entry_block >= blocks.size())
        return {BuildError::InvalidInput, 0, 0, 0, Error::None, {}};
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
    Emitter builder{scope, target, layout};
    try {
        const auto internal = [&](u64 address) {
            if (external_edges) return BlockId{};
            const auto found = std::lower_bound(blocks.begin(), blocks.end(), address,
                    [](const DecodedBlock& block, u64 value) { return block.pc < value; });
            return found != blocks.end() && found->pc == address
                    ? BlockId{static_cast<u32>(found - blocks.begin())} : BlockId{};
        };
        std::vector<u64> exits;
        if (scope == Scope::Function) {
            // Only real conditional outgoing edges need extra IR bodies. This
            // reads terminal records, not a second whole-instruction plan.
            for (const auto& block : blocks) {
                if (!block.instruction_count) continue;
                const auto& last = instructions[std::size_t{block.first_instruction} + block.instruction_count - 1];
                if (!Conditional(last.opcode) || !Direct(last)) continue;
                const u64 next = last.addr + last.size;
                for (const auto destination : {next + last.imm.addr, next})
                    if (!internal(destination)) exits.push_back(destination);
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
        PendingFlags pending;
        const auto finish = [&](u64 destination) {
            pending.Flush(builder);
            builder.PublishRegisters();
            const auto found = internal(destination);
            // Internal edges stay in native code. An installed fault-location
            // consumer recovers PC at the real fault, and actual exits still
            // publish their destination below.
            if (!found || !target.Has(TargetFeature::FaultLocations))
                StorePC(builder, layout, Constant(builder, destination));
            if (found) Require(builder.EndBlock(Terminal::Jump({unit, found})));
            else EndDispatch(builder);
        };
        const auto target_block = [&](u64 destination) {
            if (const auto found = internal(destination)) return BlockRef{unit, found};
            const auto found = std::lower_bound(exits.begin(), exits.end(), destination);
            if (found == exits.end() || *found != destination)
                throw Rejected{BuildError::InvalidInput};
            return BlockRef{unit, BlockId{static_cast<u32>(blocks.size() + (found - exits.begin()))}};
        };
        std::size_t source{};
        for (std::size_t block = 0; block < blocks.size(); ++block) {
            const auto& slice = blocks[block];
            pc = slice.pc;
            Require(builder.BeginBlock({unit, BlockId{static_cast<u32>(block)}}));
            bool ended{};
            for (std::size_t i = 0; i < slice.instruction_count; ++i) {
                const auto& instruction = instructions[slice.first_instruction + i];
                if (instruction.addr != pc) throw Rejected{BuildError::InvalidInput};
                if (instruction.flags == FLAG_NOT_DECODABLE || !instruction.size ||
                    instruction.size > 15) throw Rejected{BuildError::InvalidEncoding};
                while (source + 1 < sources.size() && sources[source + 1].pc <= pc) ++source;
                const auto& window = sources[source];
                if (pc < window.pc || pc - window.pc > window.bytes.size() ||
                    instruction.size > window.bytes.size() - static_cast<std::size_t>(pc - window.pc))
                    throw Rejected{BuildError::Truncated};
                if (block + 1 < blocks.size() && (pc >= blocks[block + 1].pc ||
                    instruction.size > blocks[block + 1].pc - pc))
                    throw Rejected{BuildError::InvalidInput};
                const auto bytes = window.bytes.subspan(static_cast<std::size_t>(pc - window.pc), instruction.size);
                if (!AllowedPrefixes(instruction, bytes)) Unsupported();
                const u64 next_pc = pc + instruction.size;
                const auto flow = META_GET_FC(instruction.meta);
                if (((flow != FC_NONE && flow != FC_CMOV) || instruction.opcode == I_CPUID ||
                     instruction.opcode == I_LDMXCSR) &&
                    i + 1 != slice.instruction_count) throw Rejected{BuildError::InvalidInput};
                if (Conditional(instruction.opcode)) {
                    if (!Direct(instruction)) Unsupported();
                    pending.Flush(builder);
                    builder.PublishRegisters();
                    const auto condition = BranchCondition(builder, instruction.opcode);
                    const u64 taken = next_pc + instruction.imm.addr;
                    if (scope == Scope::Block) {
                        if (target.Has(TargetFeature::ConditionalUniformStore)) {
                            Require(builder.Emit<OpCode::StoreUniformSelectImm>(Type::Void,
                                    {Operand::Uniform(layout.pc_offset, Type::U64), Operand::ValueOf(condition),
                                     Operand::Immediate(taken, Type::U64), Operand::Immediate(next_pc, Type::U64)}));
                        } else {
                            const auto yes = Constant(builder, taken), no = Constant(builder, next_pc);
                            const auto destination = Require(builder.Emit<OpCode::Select>(Type::U64,
                                    {Operand::ValueOf(condition), Operand::ValueOf(yes), Operand::ValueOf(no)}));
                            StorePC(builder, layout, destination);
                        }
                        EndDispatch(builder);
                    } else {
                        Require(builder.EndBlock(Terminal::Branch(condition, target_block(taken), target_block(next_pc))));
                    }
                    ended = true;
                } else
                switch (instruction.opcode) {
                    case I_ADD: Alu<OpCode::Add>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_SUB: Alu<OpCode::Sub>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_ADC: Alu<OpCode::Adc>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_SBB: Alu<OpCode::Sbb>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_INC: Alu<OpCode::Add, true>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_DEC: Alu<OpCode::Sub, true>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_CMP: Alu<OpCode::Sub>(builder, layout, instruction, pending, pc, next_pc, target, false); break;
                    case I_AND: Alu<OpCode::And>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_OR: Alu<OpCode::Or>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_XOR: Alu<OpCode::Xor>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_TEST: Alu<OpCode::And>(builder, layout, instruction, pending, pc, next_pc, target, false); break;
                    case I_NEG: Alu<OpCode::Neg>(builder, layout, instruction, pending, pc, next_pc, target); break;
                    case I_CBW: case I_CWDE: case I_CDQE: case I_CWD: case I_CDQ: case I_CQO:
                        ExtendAccumulator(builder, layout, instruction);
                        break;
                    case I_MUL: case I_IMUL:
                        if (Memory(instruction.ops[0]) || Memory(instruction.ops[1]) ||
                            (pending.recipe.result && pending.recipe.opcode != I_MUL && pending.recipe.opcode != I_IMUL))
                            pending.Flush(builder);
                        // MUL replaces only CF/OF. Keep unobserved ALU NZPA
                        // at its real producer; an older MUL recipe is simply
                        // replaced, while any already published bits remain.
                        pending.outputs &= ~FlagBits(Flag::Carry | Flag::Overflow);
                        if (!pending.outputs) pending.producer = {};
                        Multiply(builder, layout, instruction, pending, pc, next_pc, target);
                        break;
                    case I_DIV: case I_IDIV:
                        pending.Flush(builder);
                        Divide(builder, layout, instruction, pc, next_pc, target);
                        break;
                    case I_BT:
                        pending.Flush(builder);
                        BitTest(builder, layout, instruction);
                        break;
                    case I_BSF: case I_BSR: case I_TZCNT:
                        BitScan(builder, layout, instruction, bytes, pending, pc, next_pc, target);
                        break;
                    case I_POPCNT:
                        PopulationCount(builder, layout, instruction, bytes, pending, pc, next_pc, target);
                        break;
                    case I_CRC32:
                        Crc32(builder, layout, instruction, bytes, pending, pc, next_pc, target);
                        break;
                    case I_MOVSD: case I_CVTSI2SD: case I_ADDSD: case I_SUBSD: case I_MULSD: case I_DIVSD:
                    case I_UCOMISD: case I_COMISD: case I_CVTTSD2SI:
                        ScalarDouble(builder, layout, instruction, bytes, pending, pc, next_pc, target);
                        break;
                    case I_SHL: case I_SAL: case I_SHR: case I_SAR: case I_ROL: case I_ROR:
                        pending.Flush(builder);
                        Shift(builder, layout, instruction, pending, pc, next_pc, target);
                        break;
                    case I_NOT:
                        if (Memory(instruction.ops[0])) pending.Flush(builder);
                        Complement(builder, layout, instruction, pc, next_pc, target);
                        break;
                    case I_XCHG:
                        if (Memory(instruction.ops[0]) || Memory(instruction.ops[1])) pending.Flush(builder);
                        Exchange(builder, layout, instruction, pc, next_pc, target);
                        break;
                    case I_CMPXCHG:
                        CompareExchange(builder, layout, instruction, bytes, pending, pc, next_pc, target);
                        break;
                    case I_XADD:
                        ExchangeAdd(builder, layout, instruction, bytes, pending, pc, next_pc, target);
                        break;
                    case I_MOV:
                        if (Memory(instruction.ops[0]) || Memory(instruction.ops[1])) pending.Flush(builder);
                        Move(builder, layout, instruction, pc, next_pc, target); break;
                    case I_MOVZX: case I_MOVSX: case I_MOVSXD:
                        if (Memory(instruction.ops[1])) pending.Flush(builder);
                        ExtendedMove(builder, layout, instruction, pc, next_pc, target);
                        break;
                    case I_SETO: case I_SETNO: case I_SETB: case I_SETAE: case I_SETZ: case I_SETNZ:
                    case I_SETBE: case I_SETA: case I_SETS: case I_SETNS: case I_SETP: case I_SETNP:
                    case I_SETL: case I_SETGE: case I_SETLE: case I_SETG:
                        if (Memory(instruction.ops[0])) pending.Flush(builder);
                        else pending.Observe(builder, instruction);
                        ConditionalSet(builder, layout, instruction, pc, next_pc, target);
                        break;
                    case I_CMOVO: case I_CMOVNO: case I_CMOVB: case I_CMOVAE: case I_CMOVZ: case I_CMOVNZ:
                    case I_CMOVBE: case I_CMOVA: case I_CMOVS: case I_CMOVNS: case I_CMOVP: case I_CMOVNP:
                    case I_CMOVL: case I_CMOVGE: case I_CMOVLE: case I_CMOVG:
                        if (Memory(instruction.ops[1])) pending.Flush(builder);
                        else pending.Observe(builder, instruction);
                        ConditionalMove(builder, layout, instruction, pc, next_pc, target);
                        break;
                    case I_LEA:
                        if (instruction.ops[0].type != O_REG ||
                            (instruction.ops[0].size != 16 && instruction.ops[0].size != 32 && instruction.ops[0].size != 64) ||
                            !Memory(instruction.ops[1]) || instruction.ops[2].type != O_NONE ||
                            instruction.ops[3].type != O_NONE) Unsupported();
                        WriteRegister(builder, layout, instruction.ops[0],
                                Address(builder, layout, instruction, instruction.ops[1], next_pc, target, {}, nullptr,
                                        instruction.ops[0].size == 32 ? Type::U32 : Type::U64));
                        break;
                    case I_NOP: Require(builder.Emit<OpCode::Nop>(Type::Void, {})); break;
                    case I_PREFETCHT0: case I_PREFETCHT1: case I_PREFETCHT2: case I_PREFETCHNTA:
                        OneOperand(instruction);
                        if (!Memory(instruction.ops[0])) Unsupported();
                        // A non-faulting cache hint observes neither the memory
                        // nor its address registers and needs no IR operation.
                        break;
                    case I_MOVNTDQ: {
                        const auto& destination = instruction.ops[0];
                        const auto& source = instruction.ops[1];
                        if (!Memory(destination) || destination.size != 128 || !VRegister(source) || source.size != 128 ||
                            instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE ||
                            layout.fprs_offset == UINT32_MAX) Unsupported();
                        pending.Flush(builder);
                        const auto value = VRead(builder, layout, instruction, source, pc, next_pc, target);
                        VWrite<OpCode::StoreMemory>(builder, layout, instruction, destination, value, pc, next_pc, target, true);
                        break;
                    }
                    case I_SFENCE:
                        OneOperand(instruction);
                        if (instruction.ops[0].type != O_NONE) Unsupported();
                        Require(builder.Emit<OpCode::MemoryStoreFence>(Type::Void, {}));
                        break;
                    case I_PUSH: case I_POP: case I_LEAVE:
                        pending.Flush(builder);
                        PrepareFaultPC(builder, layout, pc, target);
                        if (instruction.opcode == I_PUSH) Push(builder, layout, instruction, next_pc, target);
                        else if (instruction.opcode == I_POP) Pop(builder, layout, instruction, next_pc, target);
                        else Leave(builder, layout, instruction);
                        break;
                    case I_CALL: {
                        pending.Flush(builder);
                        PrepareFaultPC(builder, layout, pc, target);
                        // Resolve CALL RSP/[RSP] before pushing the return PC.
                        const auto destination = TransferTarget(builder, layout, instruction, next_pc, target);
                        const auto stack = ReadRegister(builder, layout, R_RSP, 64);
                        const auto changed = Add(builder, stack, Operand::Immediate(u64{} - 8, Type::U64));
                        StoreMemory(builder, changed, Constant(builder, next_pc), 64);
                        WriteStack(builder, layout, changed);
                        StorePC(builder, layout, destination);
                        EndDispatch(builder);
                        ended = true;
                        break;
                    }
                    case I_JMP:
                        if (Direct(instruction)) finish(next_pc + instruction.imm.addr);
                        else {
                            pending.Flush(builder);
                            if (Memory(instruction.ops[0])) PrepareFaultPC(builder, layout, pc, target);
                            StorePC(builder, layout, TransferTarget(builder, layout, instruction, next_pc, target));
                            EndDispatch(builder);
                        }
                        ended = true;
                        break;
                    case I_RET: {
                        OneOperand(instruction);
                        u64 adjust{};
                        auto opcode_bytes = bytes;
                        // FS/GS do not change the implicit stack read.
                        while (!opcode_bytes.empty() && (opcode_bytes.front() == 0x64 || opcode_bytes.front() == 0x65))
                            opcode_bytes = opcode_bytes.subspan(1);
                        if (opcode_bytes.size() == 1 && opcode_bytes[0] == 0xc3 && instruction.ops[0].type == O_NONE) {}
                        else if (opcode_bytes.size() == 3 && opcode_bytes[0] == 0xc2 && instruction.ops[0].type == O_IMM &&
                                 instruction.ops[0].size == 16) adjust = instruction.imm.word;
                        else Unsupported();
                        pending.Flush(builder);
                        PrepareFaultPC(builder, layout, pc, target);
                        const auto stack = ReadRegister(builder, layout, R_RSP, 64);
                        const auto destination = Require(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64,
                                {Operand::ValueOf(stack)}));
                        const auto advanced = Add(builder, stack, Operand::Immediate(8 + adjust, Type::U64));
                        const _Operand rsp{O_REG, R_RSP, 64};
                        WriteRegister(builder, layout, rsp, advanced);
                        StorePC(builder, layout, destination);
                        EndDispatch(builder);
                        ended = true;
                        break;
                    }
                    case I_SYSCALL:
                        if (instruction.ops[0].type != O_NONE) Unsupported();
                        pending.Flush(builder);
                        StorePC(builder, layout, Constant(builder, next_pc));
                        EndDispatch(builder, 1);
                        ended = true;
                        break;
                    case I_LDMXCSR: case I_STMXCSR: {
                        const auto& memory = instruction.ops[0];
                        if (layout.mxcsr_offset > UINT32_MAX - 3 || !Memory(memory) || memory.size != 32 ||
                            instruction.ops[1].type != O_NONE || instruction.ops[2].type != O_NONE ||
                            instruction.ops[3].type != O_NONE) Unsupported();
                        pending.Flush(builder);
                        PrepareFaultPC(builder, layout, pc, target);
                        if (instruction.opcode == I_LDMXCSR) {
                            const auto value = ReadData(builder, layout, instruction, memory, next_pc, target);
                            Require(builder.Emit<OpCode::StoreUniform>(Type::Void,
                                    {Operand::Uniform(layout.mxcsr_offset, Type::U32), Operand::ValueOf(value, Type::U32)}));
                            StorePC(builder, layout, Constant(builder, next_pc));
                            EndDispatch(builder, 3); // Root reloads the guest FP environment before re-entry.
                            ended = true;
                        } else {
                            const auto value = Require(builder.Emit<OpCode::LoadUniform>(Type::U32,
                                    {Operand::Uniform(layout.mxcsr_offset, Type::U32)}));
                            u64 displacement{};
                            const auto address = Address(builder, layout, instruction, memory, next_pc, target, {}, &displacement);
                            StoreMemory(builder, address, value, 32, displacement);
                        }
                        break;
                    }
                    case I_CPUID: {
                        if (!layout.cpu_query || instruction.ops[0].type != O_NONE || instruction.ops[1].type != O_NONE ||
                            instruction.ops[2].type != O_NONE || instruction.ops[3].type != O_NONE) Unsupported();
                        const auto leaf = ReadRegister(builder, layout, R_EAX, 32);
                        const auto subleaf = ReadRegister(builder, layout, R_ECX, 32);
                        pending.Flush(builder);
                        builder.PublishRegisters();
                        const auto lower = Require(builder.Emit<OpCode::Cpuid>(Type::U64,
                                {Operand::ValueOf(leaf, Type::U32), Operand::ValueOf(subleaf, Type::U32),
                                 Operand::Immediate(layout.cpu_query, Type::U64)}));
                        const auto upper = Require(builder.Emit<OpCode::CpuidUpper>(Type::U64, {Operand::ValueOf(lower)}));
                        const auto ebx = Require(builder.Emit<OpCode::LsrImm>(Type::U64,
                                {Operand::ValueOf(lower), Operand::Immediate(32, Type::U8)}));
                        const auto edx = Require(builder.Emit<OpCode::LsrImm>(Type::U64,
                                {Operand::ValueOf(upper), Operand::Immediate(32, Type::U8)}));
                        WriteRegister(builder, layout, Gpr(0, 32), lower);
                        WriteRegister(builder, layout, Gpr(3, 32), ebx);
                        WriteRegister(builder, layout, Gpr(1, 32), upper);
                        WriteRegister(builder, layout, Gpr(2, 32), edx);
                        StorePC(builder, layout, Constant(builder, next_pc));
                        EndDispatch(builder); // Never turn serialization into an internal Function edge.
                        ended = true;
                        break;
                    }
                    default:
                        // Register-only vectors neither read nor write EFLAGS.
                        // Preserve pending flags until a real observer; memory
                        // forms must still expose the old flags on a fault.
                        if (Memory(instruction.ops[0]) || Memory(instruction.ops[1])) pending.Flush(builder);
                        if (Vector(builder, layout, instruction, pc, next_pc, target)) break;
                        Unsupported();
                }
                decoded += instruction.size;
                ++instruction_count;
                pc = next_pc;
            }
            if (failure.block == block) throw Rejected{failure.error, failure.detail};
            if (!ended) {
                if (slice.instruction_count) finish(pc);
                else {
                    StorePC(builder, layout, Constant(builder, pc));
                    EndDispatch(builder);
                }
            }
        }
        for (std::size_t exit = 0; exit < exits.size(); ++exit) {
            Require(builder.BeginBlock({unit, BlockId{static_cast<u32>(blocks.size() + exit)}}));
            StorePC(builder, layout, Constant(builder, exits[exit]));
            EndDispatch(builder);
        }
        if (collected) *collected = builder.CountWork();
        auto submitted = builder.Submit();
        if (!submitted)
            return {BuildError::IRRejected, pc, decoded, instruction_count, submitted.error, {}};
        return {BuildError::None, 0, decoded, instruction_count, Error::None, std::move(submitted.program)};
    } catch (const Rejected& rejected) {
        if (collected) *collected = builder.CountWork();
        return {rejected.error, pc, decoded, instruction_count, rejected.detail, {}};
    }
}
}  // namespace

BuildResult BuildDecodedIR(std::span<const _DInst> instructions, std::span<const DecodedBlock> blocks,
                           std::span<const BlockWindow> source_bytes, Scope scope, StateLayout layout,
                           TargetInfo target, WorkCounts* collected, u32 entry_block) {
    return EmitDecoded(instructions, blocks, source_bytes, scope, layout, target, collected, {}, entry_block);
}

BuildResult BuildFunctionIR(std::span<const BlockWindow> readable, u64 entry_pc, StateLayout layout,
                            DecodeLimits limits, TargetInfo target, WorkCounts* collected, DecodeTiming* timing) {
    if (collected) *collected = {};
    if (!ValidRequest(1, Scope::Function, layout, target))
        return {BuildError::InvalidInput, entry_pc, 0, 0, Error::None, {}};
    const auto decoded = DecodeFunction(readable, entry_pc, limits, timing);
    if (!decoded) {
        const auto error = decoded.error == DecodeError::InvalidInput ? BuildError::InvalidInput :
                decoded.error == DecodeError::Truncated ? BuildError::Truncated :
                decoded.error == DecodeError::OverlappingTarget ? BuildError::Unsupported : BuildError::InvalidEncoding;
        return {error, decoded.error_pc, 0, 0, Error::None, {}};
    }
    return BuildDecodedIR(decoded.instructions, decoded.blocks, readable, Scope::Function, layout,
                          target, collected, decoded.entry_block);
}

BuildResult BuildIR(std::span<const BlockWindow> windows, Scope scope, StateLayout layout,
                    TargetInfo target, WorkCounts* collected, DecodeTiming* timing, bool external_edges) {
    if (collected) *collected = {};
    if (!ValidRequest(windows.size(), scope, layout, target) || !ValidSources(windows))
        return {BuildError::InvalidInput, 0, 0, 0, Error::None, {}};
    std::vector<_DInst> instructions;
    std::vector<DecodedBlock> blocks;
    blocks.reserve(windows.size());
    DecodeFailure failure;
    for (std::size_t block = 0; block < windows.size(); ++block) {
        const auto& window = windows[block];
        blocks.push_back({window.pc, static_cast<u32>(instructions.size()), 0});
        if (failure.error != BuildError::None) continue;
        auto bytes = window.bytes;
        if (block + 1 < windows.size() && windows[block + 1].pc - window.pc < bytes.size())
            bytes = bytes.first(static_cast<std::size_t>(windows[block + 1].pc - window.pc));
        u64 pc = window.pc;
        while (!bytes.empty()) {
            _CodeInfo code{};
            code.codeOffset = pc; code.code = bytes.data();
            code.codeLen = static_cast<int>(std::min<std::size_t>(bytes.size(), 15));
            code.dt = Decode64Bits;
            _DInst instruction{};
            unsigned used{};
            _DecodeResult parsed;
            if (timing) {
                const auto begin = std::chrono::steady_clock::now();
                parsed = distorm_decompose(&code, &instruction, 1, &used);
                const auto end = std::chrono::steady_clock::now();
                timing->nanoseconds += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
                ++timing->calls;
            } else parsed = distorm_decompose(&code, &instruction, 1, &used);
            BuildError error{BuildError::None};
            if (!used) error = parsed == DECRES_INPUTERR ? BuildError::InvalidEncoding : BuildError::Truncated;
            else if (used != 1 || instruction.flags == FLAG_NOT_DECODABLE || !instruction.size ||
                     instruction.size > 15) error = BuildError::InvalidEncoding;
            else if (instruction.size > bytes.size()) error = BuildError::Truncated;
            if (error != BuildError::None) { failure = {block, error}; break; }
            if (instructions.size() == std::numeric_limits<u32>::max()) {
                failure = {block, BuildError::IRRejected, Error::CapacityOverflow}; break;
            }
            instructions.push_back(instruction);
            ++blocks.back().instruction_count;
            pc += instruction.size;
            bytes = bytes.subspan(instruction.size);
            const auto flow = META_GET_FC(instruction.meta);
            if ((flow != FC_NONE && flow != FC_CMOV) || instruction.opcode == I_CPUID ||
                instruction.opcode == I_LDMXCSR) break;
        }
    }
    return EmitDecoded(instructions, blocks, windows, scope, layout, target, collected, failure, 0, external_edges);
}

}  // namespace swift::x86
