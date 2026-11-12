#include "allocate.h"
#include "compile.h"
#include "helpers.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/backend/allocation_cfg.h"
#include "runtime/ir/uniform_effects.h"

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
        result.fpcr_may_change = false;
        result.flags_slot = UINT32_MAX;
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
                result.cold_fp_count += operation.opcode == OpCode::ArmFPBinary;
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
                    operation.opcode == OpCode::ArmFPBinary || operation.opcode == OpCode::Cpuid ||
                    operation.opcode == OpCode::ReadSystem ||
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
                       operation.opcode == OpCode::LocalCondSet)))) {
                    const auto mask = program.Operands(NodeId{i}).front().Payload();
                    home.terminal_only = operation.opcode == OpCode::CondSet ||
                                         operation.opcode == OpCode::LocalCondSet || !(mask & 16);
                }
                // Ordinary flag metadata updates the live RBX word. Only an
                // actual named snapshot consumer below needs a saved payload.
                if (operation.opcode == OpCode::Div128 || operation.opcode == OpCode::Cpuid)
                    home.auxiliary_offset = 0;  // Allocate only after the actual use walk.
            }
        }
        if (options && options->local_flags) AnalyzeFlags();
        return true;
    }

    void ObserveFPCR(const Operation& operation, NodeId id) {
        const auto opcode = operation.opcode;
        if (opcode == OpCode::CallLambda) {
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
        } else if (!options->local_flags &&
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
                const auto outputs = op.flag_outputs & 15;
                needed &= ~outputs;
                killed |= outputs;
                const auto effects = EffectsFor(op.opcode, op.Metadata());
                unsigned reads{};
                if (HasAnyEffect(effects, Effect::ReadFlags)) {
                    reads = 15;
                    const auto inputs = program.Operands(NodeId{at});
                    if (op.opcode == OpCode::Adc || op.opcode == OpCode::Sbb ||
                        op.opcode == OpCode::InvertCarry)
                        reads = 1;
                    else if (op.opcode == OpCode::TestFlags || op.opcode == OpCode::TestNotFlags)
                        reads = inputs.front().Payload() & 15;
                    else if (op.opcode == OpCode::CondSet || op.opcode == OpCode::LocalCondSet ||
                             op.opcode == OpCode::CondSelect) {
                        constexpr unsigned condition_masks[]{
                                4, 4, 1, 1, 8, 8, 2, 2, 5, 5, 10, 10, 14, 14, 0, 0};
                        const auto condition = inputs.front().Payload();
                        if (condition < 16) reads = condition_masks[condition];
                    }
                }
                if (HasAnyEffect(effects,
                                 Effect::MayFault | Effect::Unknown | Effect::External |
                                         Effect::Control))
                    reads = 15;
                if (HasAnyEffect(effects, Effect::ReadGuestState | Effect::WriteGuestState)) {
                    const auto accesses =
                            ir::UniformEffectsFor(op, program.Operands(NodeId{at}), uniforms);
                    if (accesses.all_read || accesses.all_write) reads = 15;
                    for (unsigned i = 0; i < accesses.count; ++i)
                        if (ir::Overlaps(accesses.fields[i].range, *uniforms.flags)) reads = 15;
                }
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
            if (!Reserve(bytes, bytes, id, h.stack_offset)) return false;
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
                    const auto input = program.Operands(NodeId{i}).front();
                    if (input.Kind() == OperandKind::Value &&
                        operations[input.Payload()].type == operations[i].type &&
                        !result.homes[input.Payload()].spilled &&
                        result.homes[input.Payload()].last_use == h.definition_position) {
                        for (unsigned j = 0; j < count; ++j)
                            if (bank[j] == input.Payload()) {
                                chosen = j;
                                reused = true;
                                break;
                            }
                    }
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
        if (options && options->local_flags && !Reserve(8, 8, 0, result.flags_slot)) return;
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
