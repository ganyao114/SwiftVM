#include "allocate.h"
#include "compile.h"
#include "helpers.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/backend/allocation_cfg.h"
#include "runtime/ir/flag_effects.h"

#include <algorithm>
#include <array>
#include <limits>

namespace swift::runtime::backend::x64 {
namespace {
using namespace ir;
using u32 = std::uint32_t;

constexpr std::array<unsigned, 6> pin_registers{13, 14, 15, 5};
constexpr unsigned register_count = 12;

bool SupportedType(Type type) {
    return type == ir::Type::Void || IntegerType(type) || type == ir::Type::F32 ||
           type == ir::Type::F64 || type == ir::Type::V128;
}

bool UnsupportedStorage(OpCode opcode) {
    return opcode == OpCode::AddPhi || opcode == OpCode::DefineLocal ||
           opcode == OpCode::LoadLocal || opcode == OpCode::StoreLocal;
}

bool DestructiveInput(const Operation& operation) {
    switch (operation.opcode) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::Neg:
        case OpCode::Mul:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::AndNot:
        case OpCode::LslImm:
        case OpCode::LslValue:
        case OpCode::LsrImm:
        case OpCode::LsrValue:
        case OpCode::AsrImm:
        case OpCode::AsrValue:
        case OpCode::RorImm:
        case OpCode::RorValue:
            return operation.type == Type::U32 || operation.type == Type::U64;
        case OpCode::VecAnd:
        case OpCode::Vec4And:
        case OpCode::VecOr:
        case OpCode::Vec4Or:
        case OpCode::VecXor:
        case OpCode::VecAdd:
        case OpCode::Vec4Add:
        case OpCode::VecSub:
        case OpCode::Vec4Sub:
            return operation.type == Type::V128;
        default:
            return false;
    }
}

// The private owner only borrows the original request and its actual result.
// Control flow is shared; register and auxiliary-slot policy stays here.
class Allocator {
public:
    Allocator(const Program& program,
              Scope scope,
              Allocation& result,
              unsigned pinned_slots,
              const CompileOptions* options)
            : program(program)
            , scope(scope)
            , result(result)
            , operations(program.Operations())
            , blocks(program.Blocks())
            , pinned_slots(pinned_slots)
            , options(options) {}

    void Run() {
        ResetResult(CompileError::None);
        const auto entry = program.EntryBlock();
        if (!program || program.ScopeKind() != scope || blocks.empty() ||
            entry.value >= blocks.size() || operations.size() >= NodeId::invalid ||
            (scope == Scope::Block && (blocks.size() != 1 || entry.value != 0))) {
            ResetResult(CompileError::InvalidProgram);
            return;
        }

        if (pinned_slots && pinned_slots != 2) {
            ResetResult(CompileError::UnsupportedOperand);
            return;
        }
        scalar_register_count = 0;
        for (unsigned reg : {13u, 14u, 15u, 5u, 8u, 9u}) {
            bool reserved = false;
            for (unsigned i = 0; i < pinned_slots; ++i) reserved |= pin_registers[i] == reg;
            if (!reserved) scalar_registers[scalar_register_count++] = reg;
        }

        graph = scope == Scope::Function &&
                (blocks.size() != 1 || blocks.front().terminal.kind == TerminalKind::Jump ||
                 blocks.front().terminal.kind == TerminalKind::Branch);

        if (graph)
            cfg.BuildOrder(program, result.block_order);
        else
            result.block_order.push_back(0);
        if (!InitializeHomes()) return;
        if (graph) cfg.CloseLoops(program, result.block_order);
        if (!ObserveUses() || !AllocatePayloads()) return;
        if (options && options->local_flags) AnalyzeFlags();
        AnalyzeUniformBacking();
        AssignRegisters();
    }

private:
    const Program& program;
    Scope scope;
    Allocation& result;
    std::span<const Operation> operations;
    std::span<const BlockRecord> blocks;
    backend::detail::AllocationCFG cfg;
    std::array<unsigned, 6> scalar_registers{};
    unsigned scalar_register_count{};
    unsigned pinned_slots{};
    const CompileOptions* options{};
    bool graph{};

    void ResetResult(CompileError error, NodeId node = {}, BlockId block = {}) {
        result.error = error;
        result.node = node;
        result.block = block;
        result.spill_bytes = result.call_offset = 0;
        result.has_calls = false;
        result.uses_fpcr = false;
        result.fpcr_may_change = false;
        result.flags_slot = result.condition_slot = UINT32_MAX;
        result.native_entry.clear();
        result.terminal_conditions.clear();
        result.flags_input.clear();
        result.cold_fp_count = result.cold_event_count = 0;
        result.homes.clear();
        result.block_order.clear();
    }

    bool InitializeHomes() {
        result.homes.resize(operations.size());
        u32 position{};
        for (const auto b : result.block_order) {
            const auto& body = blocks[b];
            if (graph) cfg.RecordBlock(b, position, body.terminal);
            for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i, ++position) {
                const auto& operation = operations[i];
                if (options && options->arm_fpcr != UINT32_MAX) ObserveFPCR(operation, NodeId{i});
                const bool fp_values = operation.opcode == OpCode::ArmFPTernary ||
                                       operation.opcode == OpCode::ArmFPUnary ||
                                       operation.opcode == OpCode::ArmFPConvert ||
                                       operation.opcode == OpCode::ArmFPCompare;
                result.cold_fp_count += operation.opcode == OpCode::ArmFPBinary ||
                                        operation.opcode == OpCode::CallLambda || fp_values;
                result.uses_fpcr |= operation.opcode == OpCode::ArmFPBinary ||
                                    operation.opcode == OpCode::CallLambda || fp_values ||
                                    operation.opcode == OpCode::ArmFPBitUnary;
                result.cold_event_count += operation.opcode == OpCode::GuestSafepoint ||
                                           operation.opcode == OpCode::GuestDispatchSafepoint;
                if (!SupportedType(operation.type)) {
                    ResetResult(CompileError::UnsupportedType, NodeId{i}, BlockId{b});
                    return false;
                }
                if (UnsupportedStorage(operation.opcode)) {
                    ResetResult(operation.opcode == OpCode::AddPhi
                                        ? CompileError::UnsupportedControlFlow
                                        : CompileError::UnsupportedOpcode,
                                NodeId{i},
                                BlockId{b});
                    return false;
                }
                if (operation.opcode == OpCode::CompareAndSwap128 ||
                    operation.opcode == OpCode::CallLambda ||
                    operation.opcode == OpCode::ArmFPBinary || fp_values ||
                    operation.opcode == OpCode::Cpuid || operation.opcode == OpCode::ReadSystem ||
                    operation.opcode == OpCode::ReadSystemPair ||
                    operation.opcode == OpCode::X87Op || operation.opcode == OpCode::X87Execute ||
                    operation.opcode == OpCode::Div128 ||
                    operation.opcode == OpCode::VecFRoundInt ||
                    operation.opcode == OpCode::VecFMulAdd ||
                    operation.opcode == OpCode::Sse42Str ||
                    operation.opcode == OpCode::Sse42StrExplicit ||
                    operation.opcode == OpCode::VecAesEncFast ||
                    operation.opcode == OpCode::VecAesDecFast ||
                    operation.opcode == OpCode::VecAesEncLastFast ||
                    operation.opcode == OpCode::VecAesDecLastFast ||
                    operation.opcode == OpCode::VecAesImc ||
                    operation.opcode == OpCode::VecAesEnc ||
                    operation.opcode == OpCode::VecAesDec ||
                    operation.opcode == OpCode::VecAesEncLast ||
                    operation.opcode == OpCode::VecAesDecLast ||
                    operation.opcode == OpCode::VecAesKeygenAssist ||
                    operation.opcode == OpCode::VecPclMul ||
                    operation.opcode == OpCode::VecSha256Msg1 ||
                    operation.opcode == OpCode::VecSha256Msg2 ||
                    operation.opcode == OpCode::VecSha256Rnds2)
                    result.has_calls = true;
                if (operation.opcode == OpCode::VecFCmpMask) {
                    unsigned argument{};
                    for (const auto operand : program.Operands(NodeId{i})) {
                        if (argument++ == 3) {
                            result.has_calls |= FloatingCompareNeedsCall(operand.Payload());
                            break;
                        }
                    }
                }
                auto& home = result.homes[i];
                home.definition_position = home.last_use = position;
                if (body.terminal.kind == TerminalKind::Branch && body.terminal.value.value == i &&
                    i + 1 == body.first_node + body.node_count &&
                    operation.Metadata() == OperationMetadata{} &&
                    (operation.opcode == OpCode::TestFlags ||
                     operation.opcode == OpCode::TestNotFlags ||
                     (options && options->local_flags &&
                      (operation.opcode == OpCode::CondSet ||
                       operation.opcode == OpCode::LocalCondSet ||
                       operation.opcode == OpCode::TestZero ||
                       operation.opcode == OpCode::TestNotZero ||
                       operation.opcode == OpCode::TestBit)))) {
                    const auto mask = program.Operands(NodeId{i}).front().Payload();
                    home.terminal_only = operation.opcode == OpCode::CondSet ||
                                         operation.opcode == OpCode::LocalCondSet ||
                                         operation.opcode == OpCode::TestZero ||
                                         operation.opcode == OpCode::TestNotZero ||
                                         operation.opcode == OpCode::TestBit || !(mask & 16);
                }
                // Ordinary flag metadata updates the live RBX word. Only an
                // actual named snapshot consumer below needs a saved payload.
                if (operation.opcode == OpCode::Div128 || operation.opcode == OpCode::Cpuid)
                    home.auxiliary_offset = 0;  // Allocate only after the actual use walk.
            }
        }
        return true;
    }

    void ObserveFPCR(const Operation& operation, NodeId id) {
        const auto opcode = operation.opcode;
        if (opcode == OpCode::ArmFPBinary || opcode == OpCode::ArmFPTernary ||
            opcode == OpCode::ArmFPUnary || opcode == OpCode::ArmFPConvert ||
            opcode == OpCode::ArmFPCompare) {
            const bool binary = opcode == OpCode::ArmFPBinary;
            const unsigned argument = binary || opcode == OpCode::ArmFPCompare ? 2
                                      : opcode == OpCode::ArmFPTernary         ? 3
                                                                               : 1;
            auto at = program.Operands(id).begin();
            for (unsigned i = 0; i < argument; ++i) ++at;
            const auto offsets = (*at++).Payload();
            const auto control = static_cast<u32>(offsets);
            const auto status = static_cast<u32>(binary ? (*at).Payload() : offsets >> 32);
            result.fpcr_may_change |= Overlaps({control, 4}, {status, 4});
        } else if (opcode == OpCode::CallLambda) {
            auto at = program.Operands(id).begin();
            ++at;
            const bool exact_numeric =
                    at != program.Operands(id).end() && (*at).Kind() == OperandKind::Immediate &&
                    (*at).Payload() ==
                            reinterpret_cast<std::uintptr_t>(&runtime::arm64::ExecuteNumeric);
            result.fpcr_may_change |= !exact_numeric;
        } else if (opcode == OpCode::Cpuid || opcode == OpCode::ReadSystem ||
                   opcode == OpCode::ReadSystemPair || opcode == OpCode::X87Op ||
                   opcode == OpCode::X87Execute) {
            result.fpcr_may_change = true;
        } else if (opcode == OpCode::StoreUniform || opcode == OpCode::StoreUniformSelectImm) {
            const auto field = program.Operands(id).front();
            const auto bytes = opcode == OpCode::StoreUniformSelectImm
                                       ? 8u
                                       : (TypeBits(field.TypeView()) + 7) / 8;
            result.fpcr_may_change |=
                    options->arm_fpcr_offset == UINT32_MAX ||
                    (field.Payload() < std::uint64_t{options->arm_fpcr_offset} + 4 &&
                     options->arm_fpcr_offset < field.Payload() + bytes);
        } else if (!options->private_state &&
                   HasAnyEffect(EffectsFor(opcode, operation.Metadata()), Effect::WriteMemory)) {
            result.fpcr_may_change = true;
        }
    }

    void AnalyzeFlags() {
        struct Summary {
            unsigned char gen{}, kill{};
            bool queued{true};
        };

        std::array<Summary, 8> local_facts;
        std::array<u32, 9> local_offsets{};
        std::array<u32, 16> local_predecessors;
        std::array<u32, 8> local_queue;
        std::vector<Summary> large_facts;
        std::vector<u32> large_offsets, large_predecessors, large_queue;
        const auto size = blocks.size();
        std::span<Summary> facts = local_facts;
        std::span<u32> offsets = local_offsets, predecessors = local_predecessors,
                       queue = local_queue;
        if (size > local_facts.size()) {
            large_facts.resize(size);
            large_offsets.resize(size + 1);
            large_predecessors.resize(size * 2);
            large_queue.resize(size);
            facts = large_facts;
            offsets = large_offsets;
            predecessors = large_predecessors;
            queue = large_queue;
        }
        result.flags_input.assign(size, 15);
        ir::UniformPolicy uniforms;
        uniforms.flags = ir::UniformRange{options->flags_offset, 8};
        uniforms.pc = ir::UniformRange{options->pc_offset, 8};
        for (u32 b = 0; b < size; ++b) {
            const auto& body = blocks[b];
            unsigned needed{}, killed{};
            for (u32 at = body.first_node + body.node_count; at-- > body.first_node;) {
                const auto& op = operations[at];
                auto& home = result.homes[at];
                const auto inputs = program.Operands(NodeId{at});
                unsigned outputs = op.flag_outputs & 15;
                if (op.opcode == OpCode::ArmFPCompare) outputs |= 15;
                if (op.opcode == OpCode::SetCarry || op.opcode == OpCode::InvertCarry) outputs |= 1;
                if (op.opcode == OpCode::SetOverflow) outputs |= 2;
                if (op.opcode == OpCode::ClearFlags) outputs |= inputs.front().Payload() & 15;
                if (op.opcode == OpCode::RestoreFlags || op.opcode == OpCode::SaveFlags ||
                    op.opcode == OpCode::BranchOnlyFlags) {
                    auto input = inputs.begin();
                    ++input;
                    outputs |= (*input).Payload() & 15;
                }
                const auto reads =
                        ir::FlagReads(
                                op.opcode, EffectsFor(op.opcode, op.Metadata()), inputs, uniforms) &
                        15;
                home.flag_reads = reads;
                home.flag_writes = outputs;
                needed &= ~outputs;
                killed |= outputs;
                needed |= reads;
            }
            facts[b].gen = needed;
            facts[b].kill = killed;
            const auto edge = [&](BlockId target) { ++offsets[target.value + 1]; };
            if (body.terminal.kind == TerminalKind::Jump) edge(body.terminal.first);
            if (body.terminal.kind == TerminalKind::Branch) {
                edge(body.terminal.first);
                edge(body.terminal.second);
            }
        }
        for (u32 b = 0; b < size; ++b) {
            offsets[b + 1] += offsets[b];
            queue[b] = offsets[b];
        }
        for (u32 b = 0; b < size; ++b) {
            const auto& terminal = blocks[b].terminal;
            const auto edge = [&](BlockId target) { predecessors[queue[target.value]++] = b; };
            if (terminal.kind == TerminalKind::Jump) edge(terminal.first);
            if (terminal.kind == TerminalKind::Branch) {
                edge(terminal.first);
                edge(terminal.second);
            }
        }
        for (u32 b = 0; b < size; ++b) queue[b] = b;
        std::size_t head{}, tail{}, pending = size;
        while (pending) {
            const auto b = queue[head];
            if (++head == size) head = 0;
            --pending;
            facts[b].queued = false;
            const auto& terminal = blocks[b].terminal;
            unsigned output = 15;
            if (terminal.kind == TerminalKind::Jump)
                output = result.flags_input[terminal.first.value];
            if (terminal.kind == TerminalKind::Branch)
                output = result.flags_input[terminal.first.value] |
                         result.flags_input[terminal.second.value];
            const auto input = facts[b].gen | (output & ~facts[b].kill);
            if (input == result.flags_input[b]) continue;
            result.flags_input[b] = input;
            for (auto at = offsets[b]; at < offsets[b + 1]; ++at) {
                const auto previous = predecessors[at];
                if (facts[previous].queued) continue;
                facts[previous].queued = true;
                queue[tail] = previous;
                if (++tail == size) tail = 0;
                ++pending;
            }
        }
        result.terminal_conditions.assign(size, 255);
        result.native_entry.assign(size, false);
        std::vector<bool> native_output(size);
        std::vector<u32> rank(size);
        for (u32 i = 0; i < result.block_order.size(); ++i) rank[result.block_order[i]] = i;
        for (u32 b = 0; b < size; ++b) {
            const auto& body = blocks[b];
            const auto& terminal = body.terminal;
            unsigned other = 15, term = 0;
            if (terminal.kind == TerminalKind::Jump)
                other = result.flags_input[terminal.first.value];
            if (terminal.kind == TerminalKind::Branch) {
                other = result.flags_input[terminal.first.value] |
                        result.flags_input[terminal.second.value];
                const auto& op = operations[terminal.value.value];
                if (result.homes[terminal.value.value].terminal_only) {
                    std::optional<Condition> condition;
                    if (op.opcode == OpCode::CondSet || op.opcode == OpCode::LocalCondSet)
                        condition = static_cast<Condition>(
                                program.Operands(terminal.value).front().Payload());
                    else if (op.opcode == OpCode::TestFlags || op.opcode == OpCode::TestNotFlags)
                        condition = ir::FlagCondition(
                                program.Operands(terminal.value).front().Payload(),
                                op.opcode == OpCode::TestNotFlags);
                    if (condition) result.terminal_conditions[b] = unsigned(*condition);
                }
            }
            for (u32 at = body.first_node + body.node_count; at-- > body.first_node;) {
                auto& home = result.homes[at];
                other &= ~home.flag_writes;
                term &= ~home.flag_writes;
                if (terminal.kind == TerminalKind::Branch && terminal.value.value == at &&
                    result.terminal_conditions[b] != 255)
                    term |= home.flag_reads;
                else
                    other |= home.flag_reads;
                home.flags_need = other | (term << 4);
            }
            bool native = false;
            for (u32 at = body.first_node; at < body.first_node + body.node_count; ++at) {
                const auto& op = operations[at];
                const auto code = op.opcode;
                if (op.flag_outputs == 15 && result.homes[at].auxiliary_offset == UINT32_MAX &&
                    (op.type == Type::U32 || op.type == Type::U64) &&
                    (code == OpCode::Add || code == OpCode::Sub || code == OpCode::Neg ||
                     code == OpCode::Adc || code == OpCode::Sbb || code == OpCode::And ||
                     code == OpCode::Or || code == OpCode::Xor))
                    native = true;
                else if (!(code == OpCode::Nop || code == OpCode::InvertCarry ||
                           code == OpCode::FaultLocation || code == OpCode::GuestSafepoint ||
                           code == OpCode::GuestDispatchSafepoint ||
                           (code == OpCode::LoadUniform && TypeBits(op.type) >= 8) ||
                           (code == OpCode::StoreUniform &&
                            TypeBits(program.Operands(NodeId{at}).front().TypeView()) >= 32) ||
                           (code == OpCode::LoadImm && TypeBits(op.type) >= 32) ||
                           result.homes[at].terminal_only))
                    native = false;
            }
            native_output[b] = native;
        }
        for (u32 b = 0; b < size; ++b) {
            bool backedge = false, all_native = true;
            for (auto at = offsets[b]; at < offsets[b + 1]; ++at) {
                const auto previous = predecessors[at];
                if (rank[previous] < rank[b]) continue;
                backedge = true;
                all_native &= native_output[previous];
            }
            result.native_entry[b] = backedge && all_native && result.flags_input[b];
        }
    }

    bool RecordUse(u32 definition, u32 at, u32 block = BlockId::invalid) {
        if (definition >= operations.size() || operations[definition].type == ir::Type::Void ||
            result.homes[definition].definition_position >= at)
            return false;
        auto& home = result.homes[definition];
        home.last_use = std::max(home.last_use,
                                 operations[definition].block.value == block
                                         ? at
                                         : cfg.UseEnd(home.definition_position, at));
        return true;
    }

    bool ObserveUses() {
        for (const auto b : result.block_order) {
            const auto& body = blocks[b];
            if (body.terminal.HasValue() &&
                !RecordUse(body.terminal.value.value, cfg.Start(b) + body.node_count)) {
                ResetResult(CompileError::InvalidProgram, {}, BlockId{b});
                return false;
            }
            if (body.terminal.HasValue() && operations[body.terminal.value.value].block.value != b)
                result.homes[body.terminal.value.value].terminal_only = false;
            for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
                const auto& operation = operations[i];
                const bool saved_flags = operation.opcode == OpCode::SaveFlags ||
                                         operation.opcode == OpCode::BranchOnlyFlags;
                for (const auto input : program.Operands(NodeId{i})) {
                    if (input.Kind() != OperandKind::Value) continue;
                    const auto definition = static_cast<u32>(input.Payload());
                    if (!RecordUse(definition, result.homes[i].definition_position, b)) {
                        ResetResult(CompileError::InvalidProgram, NodeId{i}, BlockId{b});
                        return false;
                    }
                    result.homes[definition].terminal_only = false;
                    if (saved_flags) result.homes[definition].auxiliary_offset = 0;
                }
            }
        }

        return true;
    }

    void AnalyzeUniformBacking() {
        if (!options) return;
        UniformPolicy policy;
        policy.flags = UniformRange{options->flags_offset, 8};
        policy.pc = UniformRange{options->pc_offset, 8};
        policy.numeric_control = UniformRange{options->mxcsr_offset, 4};
        for (const auto b : result.block_order) {
            const auto& body = blocks[b];
            if (!body.node_count) continue;
            std::array<u32, 128> next_write;
            next_write.fill(UINT32_MAX);
            u32 barrier = UINT32_MAX;
            const auto end = result.homes[body.first_node].definition_position + body.node_count;
            for (u32 at = body.first_node + body.node_count; at-- > body.first_node;) {
                const auto& op = operations[at];
                auto& home = result.homes[at];
                const auto inputs = program.Operands(NodeId{at});
                if (op.opcode == OpCode::StoreUniform && op.Metadata() == OperationMetadata{}) {
                    auto input = inputs.begin();
                    const auto field = *input++, value = *input;
                    if (value.Kind() == OperandKind::Value && value.Payload() < at &&
                        at - value.Payload() <= 3 &&
                        operations[value.Payload()].block == op.block) {
                        const auto& producer = operations[value.Payload()];
                        auto& backing = result.homes[value.Payload()];
                        const auto bits = TypeBits(producer.type),
                                   bytes = TypeBits(field.TypeView()) / 8;
                        const auto view =
                                value.TypeView() == Type::Void ? producer.type : value.TypeView();
                        const bool matching =
                                producer.type == Type::V128
                                        ? view == Type::V128 && field.TypeView() == Type::V128
                                        : (producer.type == Type::U32 ||
                                           producer.type == Type::U64) &&
                                                  view != Type::V128 &&
                                                  (field.TypeView() == Type::U32 ||
                                                   field.TypeView() == Type::U64);
                        const UniformRange range{u32(field.Payload()), bytes};
                        bool adjacent =
                                producer.opcode == OpCode::VecLoadConst ||
                                !HasAnyEffect(EffectsFor(producer.opcode, producer.Metadata()),
                                              Effect::Control);
                        for (auto between = value.Payload() + 1; between < at; ++between)
                            adjacent &= operations[between].opcode == OpCode::Nop &&
                                        operations[between].Metadata() == OperationMetadata{};
                        const bool pinned =
                                pinned_slots &&
                                (options->pinned_uniform_offset == UINT32_MAX ||
                                 Overlaps(range,
                                          {options->pinned_uniform_offset, pinned_slots * 8}));
                        bool ordinary = !pinned;
                        for (auto offset : {options->flags_offset,
                                            options->pc_offset,
                                            options->mxcsr_offset,
                                            options->arm_fpcr_offset,
                                            options->arm_fpsr_offset})
                            ordinary &= offset == UINT32_MAX || !Overlaps(range, {offset, 8});
                        if (adjacent && ordinary && field.Payload() < 1024 && bytes &&
                            range.offset + bytes <= 1024 && bits <= bytes * 8 &&
                            TypeBits(view) >= bits && matching && backing.last_use <= end) {
                            auto limit = barrier;
                            for (auto slot = range.offset / 8;
                                 slot <= (range.offset + bytes - 1) / 8;
                                 ++slot)
                                limit = std::min(limit, next_write[slot]);
                            if (backing.last_use < limit) {
                                backing.uniform_offset = range.offset;
                                backing.uniform_store_bytes = bytes;
                            }
                        }
                    }
                }
                if (op.opcode == OpCode::LoadUniform && !home.uniform_store_bytes &&
                    home.last_use <= end &&
                    (op.type == Type::U32 || op.type == Type::U64 || op.type == Type::V128)) {
                    const auto field = inputs.front();
                    const UniformRange range{u32(field.Payload()), TypeBits(op.type) / 8};
                    const bool pinned =
                            pinned_slots &&
                            (options->pinned_uniform_offset == UINT32_MAX ||
                             Overlaps(range, {options->pinned_uniform_offset, pinned_slots * 8}));
                    if (field.Payload() < 1024 && range.offset + range.bytes <= 1024 && !pinned &&
                        !Overlaps(range, *policy.flags)) {
                        auto limit = barrier;
                        for (auto slot = range.offset / 8;
                             slot <= (range.offset + range.bytes - 1) / 8;
                             ++slot)
                            limit = std::min(limit, next_write[slot]);
                        if (home.last_use < limit) home.uniform_offset = range.offset;
                    }
                }
                const auto effects = EffectsFor(op.opcode, op.Metadata());
                if (op.opcode == OpCode::VecLoadConst || op.opcode == OpCode::VecSharedZero)
                    continue;
                if (HasAnyEffect(effects, Effect::Unknown | Effect::External | Effect::Control) ||
                    (!options->private_state && HasAnyEffect(effects, Effect::WriteMemory))) {
                    barrier = home.definition_position;
                    continue;
                }
                const auto accesses = UniformEffectsFor(op, inputs, policy);
                if (accesses.all_write) barrier = home.definition_position;
                for (unsigned i = 0; i < accesses.count; ++i) {
                    const auto& access = accesses.fields[i];
                    if (!access.write || !access.range.bytes || access.range.offset >= 1024)
                        continue;
                    const auto last = std::min<std::uint64_t>(
                            1024, std::uint64_t{access.range.offset} + access.range.bytes);
                    for (auto slot = access.range.offset / 8; slot <= (last - 1) / 8; ++slot)
                        next_write[slot] = home.definition_position;
                }
            }
        }
    }

    bool Reserve(u32 bytes, u32 alignment, u32 node, u32& offset) {
        constexpr auto limit = static_cast<u32>(std::numeric_limits<std::int32_t>::max()) - 16;
        if (result.spill_bytes > limit - bytes - alignment) {
            ResetResult(CompileError::FrameTooLarge, NodeId{node}, operations[node].block);
            return false;
        }
        offset = (result.spill_bytes + alignment - 1) & ~(alignment - 1);
        result.spill_bytes = offset + bytes;
        return true;
    }

    bool AllocatePayloads() {
        for (u32 i = 0; i < operations.size(); ++i)
            if (result.homes[i].auxiliary_offset != UINT32_MAX &&
                !Reserve(16, 16, i, result.homes[i].auxiliary_offset))
                return false;

        return true;
    }

    void AssignRegisters() {
        std::array<std::array<u32, register_count>, 2> banks;
        for (auto& bank : banks) bank.fill(NodeId::invalid);
        const auto spill = [&](u32 id, auto& residents) {
            auto& h = result.homes[id];
            unsigned bytes = operations[id].type == ir::Type::V128 ? 16 : 8;
            if (h.uniform_offset == UINT32_MAX && !Reserve(bytes, bytes, id, h.stack_offset))
                return false;
            h.spilled = true;
            for (auto& r : residents)
                if (r == id) r = NodeId::invalid;
            return true;
        };
        for (auto b : result.block_order) {
            const auto& body = blocks[b];
            for (u32 i = body.first_node; i < body.first_node + body.node_count; ++i) {
                if (operations[i].type == ir::Type::Void || result.homes[i].terminal_only) continue;
                const bool vector = operations[i].type == ir::Type::V128;
                auto& bank = banks[vector];
                const unsigned count = vector ? 12 : scalar_register_count;
                auto& h = result.homes[i];
                unsigned chosen = count;
                bool reused{};
                if (DestructiveInput(operations[i])) {
                    const auto reuse = [&](CellView input) {
                        if (input.Kind() != OperandKind::Value ||
                            operations[input.Payload()].type != operations[i].type ||
                            result.homes[input.Payload()].spilled ||
                            result.homes[input.Payload()].last_use != h.definition_position)
                            return count;
                        for (unsigned j = 0; j < count; ++j)
                            if (bank[j] == input.Payload()) return j;
                        return count;
                    };
                    auto inputs = program.Operands(NodeId{i}).begin();
                    chosen = reuse(*inputs++);
                    const auto opcode = operations[i].opcode;
                    if (chosen == count && !vector && operations[i].operand_count == 2 &&
                        (opcode == OpCode::Add || opcode == OpCode::And || opcode == OpCode::Or ||
                         opcode == OpCode::Xor)) {
                        chosen = reuse(*inputs);
                        h.reuse_rhs = chosen != count;
                    }
                    reused = chosen != count;
                }
                u32 far = h.last_use;
                for (unsigned j = 0; !reused && j < count; ++j) {
                    if (bank[j] != NodeId::invalid &&
                        result.homes[bank[j]].last_use < h.definition_position)
                        bank[j] = NodeId::invalid;
                    if (bank[j] == NodeId::invalid) {
                        chosen = j;
                        break;
                    }
                    if (result.homes[bank[j]].last_use > far) {
                        far = result.homes[bank[j]].last_use;
                        chosen = j;
                    }
                }
                if (chosen == count) {
                    if (!spill(i, bank)) return;
                    continue;
                }
                if (!reused && bank[chosen] != NodeId::invalid && !spill(bank[chosen], bank))
                    return;
                h.reg = vector ? 2 + chosen : scalar_registers[chosen];
                bank[chosen] = i;
            }
        }
        if (options && options->local_flags &&
            (!Reserve(8, 8, 0, result.flags_slot) || !Reserve(1, 1, 0, result.condition_slot)))
            return;
        result.spill_bytes = (result.spill_bytes + 15) & ~15u;
        result.call_offset = result.spill_bytes;
        if (result.has_calls) result.spill_bytes += 320;
    }
};
}  // namespace

void AllocateBlock(const ir::Program& program,
                   Allocation& allocation,
                   unsigned pinned_slots,
                   const CompileOptions* options) {
    Allocator(program, ir::Scope::Block, allocation, pinned_slots, options).Run();
}

void AllocateFunction(const ir::Program& program,
                      Allocation& allocation,
                      unsigned pinned_slots,
                      const CompileOptions* options) {
    Allocator(program, ir::Scope::Function, allocation, pinned_slots, options).Run();
}

Allocation AllocateBlock(const ir::Program& program, unsigned pinned_slots) {
    Allocation result;
    AllocateBlock(program, result, pinned_slots);
    return result;
}

Allocation AllocateFunction(const ir::Program& program, unsigned pinned_slots) {
    Allocation result;
    AllocateFunction(program, result, pinned_slots);
    return result;
}

}  // namespace swift::runtime::backend::x64
