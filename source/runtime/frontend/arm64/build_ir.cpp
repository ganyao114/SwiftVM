#include <algorithm>
#include <array>
#include <vector>
#include "build_ir.h"
#include "runtime/frontend/timing.h"
#include "semantics.h"
#include "sink.h"

namespace swift::arm64 {
namespace {
using namespace runtime::ir;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using Sem = detail::Semantics<detail::IRSink>;

void Require(Error e) {
    if (e != Error::None) throw detail::Rejected{BuildError::IRRejected, e};
}

Value Require(AppendResult r) {
    if (!r) throw detail::Rejected{BuildError::IRRejected, r.error};
    return r.GetValue();
}

Value Constant(detail::IRSink& sink, u64 bits) {
    return Require(sink.Emit<OpCode::LoadImm>(Type::U64, {Operand::Immediate(bits, Type::U64)}));
}

BuildResult EmitDecoded(std::span<const Instruction> instructions,
                        std::span<const DecodedBlock> blocks,
                        Scope scope,
                        StateLayout layout,
                        TargetInfo target,
                        WorkCounts* counts,
                        u32 entry,
                        Program* reuse,
                        bool external,
                        std::vector<runtime::SourceSpan>* sources,
                        std::vector<u64>* continuations) {
    if (counts) *counts = {};
    if (blocks.empty() || entry >= blocks.size() || (scope == Scope::Block && blocks.size() != 1) ||
        blocks.size() >= BlockId::invalid)
        return {BuildError::InvalidInput};
    detail::IRSink sink{scope, target, reuse ? std::move(*reuse) : Program{}};
    std::size_t total{}, bytes{};
    u64 pc = blocks[entry].pc;
    try {
        const auto internal = [&](u64 address) {
            if (external || scope == Scope::Block) return BlockId{};
            const auto i = std::lower_bound(
                    blocks.begin(), blocks.end(), address, [](const auto& b, u64 p) {
                        return b.pc < p;
                    });
            return i != blocks.end() && i->pc == address && i->instruction_count
                           ? BlockId{u32(i - blocks.begin())}
                           : BlockId{};
        };
        std::array<u64, 64> local_exits;
        std::size_t exit_count{};
        std::vector<u64> heap_exits;
        const auto push_exit = [&](u64 addr) {
            if (heap_exits.empty() && exit_count < local_exits.size()) {
                local_exits[exit_count++] = addr;
                return;
            }
            if (heap_exits.empty()) {
                heap_exits.reserve(local_exits.size() * 2);
                heap_exits.assign(local_exits.begin(), local_exits.begin() + exit_count);
            }
            heap_exits.push_back(addr);
        };
        if (scope == Scope::Function && !external)
            for (const auto& b : blocks) {
                if (!b.instruction_count) continue;
                const auto& last = instructions[b.first_instruction + b.instruction_count - 1];
                if (last.Flow() != FlowKind::Conditional) continue;
                u64 taken{};
                if (last.RelativeTarget(taken)) {
                    if (!internal(taken)) push_exit(taken);
                    if (!internal(last.PC() + 4)) push_exit(last.PC() + 4);
                }
            }
        u64* exits_begin = heap_exits.empty() ? local_exits.data() : heap_exits.data();
        std::size_t exits_size = heap_exits.empty() ? exit_count : heap_exits.size();
        std::sort(exits_begin, exits_begin + exits_size);
        exits_size = static_cast<std::size_t>(
                std::unique(exits_begin, exits_begin + exits_size) - exits_begin);
        const std::span<const u64> exits{exits_begin, exits_size};
        for (std::size_t i = 0; i < blocks.size() + exits.size(); ++i) {
            const auto r = sink.CreateBlock();
            Require(r.error);
        }
        const auto owner = sink.UnitId();
        Require(sink.SetEntry({owner, BlockId{entry}}));
        const auto store = [&](Value value) {
            Require(sink.Emit<OpCode::StoreUniform>(
                    Type::Void,
                    {Operand::Uniform(layout.pc_offset, Type::U64), Operand::ValueOf(value)}));
        };
        const auto dispatch = [&](u64 address) {
            store(Constant(sink, address));
            Require(sink.EndBlock(Terminal::Dispatch()));
        };
        const auto jump = [&](u64 address) {
            if (const auto id = internal(address))
                Require(sink.EndBlock(Terminal::Jump({owner, id})));
            else
                dispatch(address);
        };
        const auto target_block = [&](u64 address) {
            if (const auto id = internal(address)) return BlockRef{owner, id};
            const auto i = std::lower_bound(exits.begin(), exits.end(), address);
            return BlockRef{owner, BlockId{u32(blocks.size() + std::size_t(i - exits.begin()))}};
        };
        Sem semantics{sink, layout, target};
        for (u32 n = 0; n < blocks.size(); ++n) {
            const auto& b = blocks[n];
            pc = b.pc;
            if (b.first_instruction > instructions.size() ||
                b.instruction_count > instructions.size() - b.first_instruction)
                throw detail::Rejected{BuildError::InvalidInput};
            Require(sink.BeginBlock({owner, BlockId{n}}));
            if (scope == Scope::Function && n != entry && b.instruction_count &&
                target.Has(TargetFeature::ExternalEntries))
                Require(sink.Emit<OpCode::EntryPoint>(Type::Void,
                                                      {Operand::Immediate(pc, Type::U64)}));
            bool ended{};
            for (u32 j = 0; j < b.instruction_count; ++j) {
                const auto& inst = instructions[b.first_instruction + j];
                if (inst.PC() != pc) throw detail::Rejected{BuildError::InvalidInput};
                semantics.Execute(inst,
                                  j && j % runtime::frontend::GuestEventInstructionBudget == 0);
                ++total;
                bytes += 4;
                pc += 4;
                if (continuations &&
                    (inst.Flow() == FlowKind::Call || inst.Flow() == FlowKind::Service ||
                     (inst.Boundary() && inst.Flow() == FlowKind::Linear)))
                    continuations->push_back(pc);
                const auto& control = semantics.control;
                using K = Sem::Control::Kind;
                if (control.kind == K::Jump) {
                    jump(control.yes);
                    ended = true;
                } else if (control.kind == K::Conditional) {
                    if (scope == Scope::Block || external) {
                        if (target.Has(TargetFeature::ConditionalUniformStore))
                            Require(sink.Emit<OpCode::StoreUniformSelectImm>(
                                    Type::Void,
                                    {Operand::Uniform(layout.pc_offset, Type::U64),
                                     Operand::ValueOf(control.condition.raw,
                                                      control.condition.view),
                                     Operand::Immediate(control.yes, Type::U64),
                                     Operand::Immediate(control.no, Type::U64)}));
                        else {
                            const auto yes = Constant(sink, control.yes),
                                       no = Constant(sink, control.no);
                            store(Require(sink.Emit<OpCode::Select>(
                                    Type::U64,
                                    {Operand::ValueOf(control.condition.raw),
                                     Operand::ValueOf(yes),
                                     Operand::ValueOf(no)})));
                        }
                        Require(sink.EndBlock(Terminal::Dispatch()));
                    } else
                        Require(sink.EndBlock(Terminal::Branch(control.condition.raw,
                                                               target_block(control.yes),
                                                               target_block(control.no))));
                    ended = true;
                } else if (control.kind == K::Dispatch) {
                    store(control.destination.IsImm() ? Constant(sink, control.destination.imm.bits)
                                                      : control.destination.value.raw);
                    Require(sink.EndBlock(Terminal::Dispatch()));
                    ended = true;
                } else if (control.kind == K::Service) {
                    store(Constant(sink, control.yes));
                    Require(sink.EndBlock(Terminal::Return(Constant(sink, control.service))));
                    ended = true;
                }
                if (ended) {
                    if (j + 1 != b.instruction_count)
                        throw detail::Rejected{BuildError::InvalidInput};
                    break;
                }
            }
            if (!ended) jump(pc);
            if (sources && pc > b.pc) sources->push_back({b.pc, pc - b.pc});
        }
        for (std::size_t i = 0; i < exits.size(); ++i) {
            Require(sink.BeginBlock({owner, BlockId{u32(blocks.size() + i)}}));
            dispatch(exits[i]);
        }
        if (counts) *counts = sink.CountWork();
        auto submitted = sink.Submit();
        Require(submitted.error);
        return {BuildError::None, 0, bytes, total, Error::None, std::move(submitted.program)};
    } catch (const detail::Rejected& failure) {
        if (counts) *counts = sink.CountWork();
        return {failure.error, pc, bytes, total, failure.detail, {}};
    }
}
}  // namespace

BuildResult BuildDecodedIR(std::span<const Instruction> instructions,
                           std::span<const DecodedBlock> blocks,
                           std::span<const BlockWindow>,
                           Scope scope,
                           StateLayout layout,
                           TargetInfo target,
                           WorkCounts* counts,
                           u32 entry,
                           Program* reusable,
                           std::vector<runtime::SourceSpan>* sources,
                           std::vector<u64>* continuations) {
    return EmitDecoded(instructions,
                       blocks,
                       scope,
                       layout,
                       target,
                       counts,
                       entry,
                       reusable,
                       false,
                       sources,
                       continuations);
}

BuildResult BuildFunctionIR(std::span<const BlockWindow> windows,
                            u64 entry,
                            StateLayout layout,
                            DecodeLimits limits,
                            TargetInfo target,
                            WorkCounts* counts,
                            DecodeTiming* timing,
                            Program* reuse,
                            ReadyEntryLookup ready,
                            std::vector<runtime::SourceSpan>* sources,
                            std::vector<u64>* continuations) {
    const auto decoded = [&] {
        runtime::frontend::ScopedBuildTimer formation{
                timing && timing->stages ? &timing->formation_ns : nullptr};
        return DecodeFunction(windows, entry, limits, timing, ready, nullptr);
    }();
    if (!decoded) {
        if (counts) *counts = {};
        return {BuildError::FormationRejected, decoded.error_pc};
    }
    runtime::frontend::ScopedBuildTimer ir_birth{timing && timing->stages ? &timing->ir_birth_ns
                                                                          : nullptr};
    return EmitDecoded(decoded.instructions,
                       decoded.blocks,
                       Scope::Function,
                       layout,
                       target,
                       counts,
                       decoded.entry_block,
                       reuse,
                       false,
                       sources,
                       continuations);
}

BuildResult BuildIR(std::span<const BlockWindow> windows,
                    Scope scope,
                    StateLayout layout,
                    TargetInfo target,
                    WorkCounts* counts,
                    DecodeTiming* timing,
                    bool external,
                    Program* reuse,
                    std::vector<runtime::SourceSpan>* sources,
                    std::vector<u64>* continuations) {
    if (windows.empty() || (scope == Scope::Block && windows.size() != 1))
        return {BuildError::InvalidInput};
    std::vector<Instruction> instructions;
    std::vector<DecodedBlock> blocks;
    blocks.reserve(windows.size());
    instructions.reserve(scope == Scope::Block
                                 ? std::min<std::size_t>(windows[0].bytes.size() / 4, 64)
                                 : std::min<std::size_t>(windows.size() * 8, 256));
    {
        runtime::frontend::ScopedBuildTimer formation{
                timing && timing->stages ? &timing->formation_ns : nullptr};
        for (const auto& w : windows) {
            DecodedBlock b{w.pc, u32(instructions.size()), 0};
            std::size_t offset{};
            while (offset < w.bytes.size()) {
                const auto decoded = DecodeOne(w.bytes.subspan(offset), w.pc + offset, timing);
                if (!decoded) {
                    if (scope == Scope::Block && b.instruction_count &&
                        target.Has(TargetFeature::GuestEvents))
                        break;
                    return {decoded.error == DecodeError::Truncated ? BuildError::Truncated
                                                                    : BuildError::InvalidEncoding,
                            w.pc + offset};
                }
                instructions.push_back(decoded.instruction);
                ++b.instruction_count;
                offset += 4;
                if (decoded.instruction.Boundary()) break;
            }
            blocks.push_back(b);
        }
    }
    runtime::frontend::ScopedBuildTimer ir_birth{timing && timing->stages ? &timing->ir_birth_ns
                                                                          : nullptr};
    return EmitDecoded(instructions,
                       blocks,
                       scope,
                       layout,
                       target,
                       counts,
                       0,
                       reuse,
                       external,
                       sources,
                       continuations);
}
}  // namespace swift::arm64
