#include <algorithm>
#include <vector>
#include "build_ir.h"
#include "runtime/exit.h"
#include "runtime/frontend/guest_events.h"
#include "runtime/frontend/timing.h"
#include "sink.h"

namespace swift::arm32 {
namespace {
using namespace runtime::ir;
using Sem = detail::Semantics<detail::IRSink>;

void Require(Error error) {
    if (error != Error::None) throw detail::Rejected{BuildError::IRRejected, error};
}

Value Require(AppendResult result) {
    if (!result) throw detail::Rejected{BuildError::IRRejected, result.error};
    return result.GetValue();
}

// Both streaming Block and cached Function decoding feed this one Program owner.
class Emitter {
public:
    Emitter(Scope scope,
            StateLayout layout,
            TargetInfo target,
            Program* reuse,
            std::span<const DecodedBlock> blocks,
            std::span<const Instruction> instructions,
            u32 entry,
            bool external,
            Profile profile)
            : sink(scope, target, reuse ? std::move(*reuse) : Program{})
            , semantics(sink, layout, target, profile)
            , blocks(blocks)
            , scope(scope)
            , layout(layout)
            , target(target)
            , external(external)
            , entry_index(entry) {
        if (scope == Scope::Function && !external) {
            for (const auto& block : blocks) {
                if (!block.instruction_count) continue;
                const auto& last =
                        instructions[block.first_instruction + block.instruction_count - 1];
                u64 destination{};
                if ((last.Flow() == FlowKind::Conditional || last.Condition() != Cond::AL) &&
                    last.RelativeTarget(destination)) {
                    if (!Internal(destination)) exits.push_back(destination);
                    if (!Internal(last.NextPC())) exits.push_back(last.NextPC());
                }
            }
            std::sort(exits.begin(), exits.end());
            exits.erase(std::unique(exits.begin(), exits.end()), exits.end());
        }
        for (std::size_t i = 0; i < blocks.size() + exits.size(); ++i)
            Require(sink.CreateBlock().error);
        owner = sink.UnitId();
        Require(sink.SetEntry({owner, BlockId{entry}}));
    }

    void Begin(u32 index) {
        const auto& block = blocks[index];
        pc = block.pc;
        physical_end = Location{pc}.Address();
        Require(sink.BeginBlock({owner, BlockId{index}}));
        if (scope == Scope::Function && index != entry_index && block.instruction_count &&
            target.Has(TargetFeature::ExternalEntries))
            Require(sink.Emit<OpCode::EntryPoint>(Type::Void, {Operand::Immediate(pc, Type::U64)}));
    }

    bool Instruction(const arm32::Instruction& instruction, u32 position) {
        pc = instruction.PC();
        semantics.Execute(
                instruction,
                position && position % runtime::frontend::GuestEventInstructionBudget == 0);
        ++total;
        bytes += instruction.Length();
        physical_end = u64(instruction.Address()) + instruction.Length();
        pc = instruction.NextPC();
        const auto& control = semantics.control;
        using Kind = Sem::Control::Kind;
        switch (control.kind) {
            case Kind::None:
                return false;
            case Kind::Jump:
                Jump(control.yes);
                break;
            case Kind::Conditional:
                if (scope == Scope::Function && !external)
                    Require(sink.EndBlock(Terminal::Branch(control.condition.raw,
                                                           TargetBlock(control.yes),
                                                           TargetBlock(control.no))));
                else {
                    StoreSelected(
                            control.condition.raw, control.condition.view, control.yes, control.no);
                    Require(sink.EndBlock(Terminal::Dispatch()));
                }
                break;
            case Kind::Dispatch:
                Store(control.destination.IsImm() ? Constant(control.destination.imm.bits)
                                                  : control.destination.value.raw);
                Require(sink.EndBlock(Terminal::Dispatch()));
                break;
            case Kind::Service:
                if (control.predicated) {
                    StoreSelected(control.condition.raw, control.condition.view, control.yes, pc);
                    const auto result = Require(sink.Emit<OpCode::Select>(
                            Type::U64,
                            {Operand::ValueOf(control.condition.raw, control.condition.view),
                             Operand::ValueOf(Constant(control.service)),
                             Operand::ValueOf(Constant(
                                     runtime::ExitValue(runtime::NativeExit::Continue)))}));
                    Require(sink.EndBlock(Terminal::Return(result)));
                } else {
                    Store(Constant(control.yes));
                    Require(sink.EndBlock(Terminal::Return(Constant(control.service))));
                }
                break;
        }
        return true;
    }

    void Finish(bool ended) {
        if (!ended) Jump(pc);
    }

    void FinishExits() {
        for (std::size_t i = 0; i < exits.size(); ++i) {
            Require(sink.BeginBlock({owner, BlockId{u32(blocks.size() + i)}}));
            Dispatch(exits[i]);
        }
    }

    BuildResult Submit(WorkCounts* counts) {
        if (counts) *counts = sink.CountWork();
        auto result = sink.Submit();
        Require(result.error);
        return {BuildError::None, 0, bytes, total, Error::None, std::move(result.program)};
    }

    detail::IRSink sink;
    Sem semantics;
    u64 pc{}, physical_end{};
    std::size_t total{}, bytes{};

private:
    BlockId Internal(u64 address) const {
        if (external || scope == Scope::Block) return {};
        const auto it = std::lower_bound(
                blocks.begin(), blocks.end(), address, [](const auto& block, u64 pc) {
                    return block.pc < pc;
                });
        return it != blocks.end() && it->pc == address && it->instruction_count
                       ? BlockId{u32(it - blocks.begin())}
                       : BlockId{};
    }

    BlockRef TargetBlock(u64 address) const {
        if (const auto internal = Internal(address)) return {owner, internal};
        const auto it = std::lower_bound(exits.begin(), exits.end(), address);
        if (it == exits.end() || *it != address) throw detail::Rejected{BuildError::InvalidInput};
        return {owner, BlockId{u32(blocks.size() + (it - exits.begin()))}};
    }

    Value Constant(u64 bits) {
        return Require(
                sink.Emit<OpCode::LoadImm>(Type::U64, {Operand::Immediate(bits, Type::U64)}));
    }

    void Store(Value value) {
        Require(sink.Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(layout.pc_offset, Type::U64), Operand::ValueOf(value)}));
    }

    void StoreSelected(Value predicate, Type view, u64 yes, u64 no) {
        if (target.Has(TargetFeature::ConditionalUniformStore))
            Require(sink.Emit<OpCode::StoreUniformSelectImm>(
                    Type::Void,
                    {Operand::Uniform(layout.pc_offset, Type::U64),
                     Operand::ValueOf(predicate, view),
                     Operand::Immediate(yes, Type::U64),
                     Operand::Immediate(no, Type::U64)}));
        else
            Store(Require(sink.Emit<OpCode::Select>(Type::U64,
                                                    {Operand::ValueOf(predicate, view),
                                                     Operand::ValueOf(Constant(yes)),
                                                     Operand::ValueOf(Constant(no))})));
    }

    void Dispatch(u64 address) {
        Store(Constant(address));
        Require(sink.EndBlock(Terminal::Dispatch()));
    }

    void Jump(u64 address) {
        if (const auto internal = Internal(address))
            Require(sink.EndBlock(Terminal::Jump({owner, internal})));
        else
            Dispatch(address);
    }

    std::span<const DecodedBlock> blocks;
    Scope scope;
    StateLayout layout;
    TargetInfo target;
    bool external;
    u32 entry_index;
    u64 owner{};
    std::vector<u64> exits;
};

void Continuation(const Instruction& instruction, std::vector<u64>* continuations) {
    if (!continuations) return;
    const auto flow = instruction.Flow();
    if (flow == FlowKind::Call || flow == FlowKind::Service ||
        (instruction.Boundary() && flow == FlowKind::Linear))
        continuations->push_back(instruction.NextPC());
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
                        std::vector<u64>* continuations,
                        Profile profile) {
    if (counts) *counts = {};
    if (blocks.empty() || entry >= blocks.size() || (scope == Scope::Block && blocks.size() != 1) ||
        blocks.size() >= BlockId::invalid)
        return {BuildError::InvalidInput};
    for (const auto& block : blocks)
        if (block.first_instruction > instructions.size() ||
            block.instruction_count > instructions.size() - block.first_instruction)
            return {BuildError::InvalidInput, block.pc};
    u64 pc = blocks[entry].pc;
    try {
        Emitter emitter{
                scope, layout, target, reuse, blocks, instructions, entry, external, profile};
        for (u32 index = 0; index < blocks.size(); ++index) {
            const auto& block = blocks[index];
            emitter.Begin(index);
            bool ended{};
            for (u32 i = 0; i < block.instruction_count; ++i) {
                const auto& instruction = instructions[block.first_instruction + i];
                pc = instruction.PC();
                if (pc != emitter.pc) throw detail::Rejected{BuildError::InvalidInput};
                ended = emitter.Instruction(instruction, i);
                Continuation(instruction, continuations);
                // Operand validity can terminate before the byte-level block
                // end. Submit the valid prefix and its guest exception.
                if (ended) break;
            }
            emitter.Finish(ended);
            const auto address = Location{block.pc}.Address();
            if (sources && emitter.physical_end > address)
                sources->push_back({address, emitter.physical_end - address});
        }
        emitter.FinishExits();
        return emitter.Submit(counts);
    } catch (const detail::Rejected& failure) {
        return {failure.error, pc, 0, 0, failure.detail, {}};
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
                           Program* reuse,
                           std::vector<runtime::SourceSpan>* sources,
                           std::vector<u64>* continuations,
                           Profile profile) {
    return EmitDecoded(instructions,
                       blocks,
                       scope,
                       layout,
                       target,
                       counts,
                       entry,
                       reuse,
                       false,
                       sources,
                       continuations,
                       profile);
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
                            std::vector<u64>* continuations,
                            Profile profile) {
    const auto decoded = [&] {
        runtime::frontend::ScopedBuildTimer timer{timing && timing->stages ? &timing->formation_ns
                                                                           : nullptr};
        return DecodeFunction(windows, entry, limits, timing, ready);
    }();
    if (!decoded) return {BuildError::FormationRejected, decoded.error_pc};
    runtime::frontend::ScopedBuildTimer timer{timing && timing->stages ? &timing->ir_birth_ns
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
                       continuations,
                       profile);
}

BuildResult BuildIR(std::span<const BlockWindow> windows,
                    u64 entry,
                    Scope scope,
                    StateLayout layout,
                    TargetInfo target,
                    WorkCounts* counts,
                    DecodeTiming* timing,
                    bool external,
                    Program* reuse,
                    std::vector<runtime::SourceSpan>* sources,
                    std::vector<u64>* continuations,
                    Profile profile) {
    if (counts) *counts = {};
    if (windows.empty()) return {BuildError::InvalidInput, entry};
    if (scope == Scope::Function) {
        const auto decoded = DecodeFunction(windows, entry, {}, timing);
        if (!decoded) return {BuildError::FormationRejected, decoded.error_pc};
        return EmitDecoded(decoded.instructions,
                           decoded.blocks,
                           scope,
                           layout,
                           target,
                           counts,
                           decoded.entry_block,
                           reuse,
                           external,
                           sources,
                           continuations,
                           profile);
    }
    // The common JIT Block path decodes directly into the final sink. It never
    // creates a decoded vector, a visitor runtime table, or a second Program.
    const DecodedBlock block{entry, 0, 0};
    auto pc = entry;
    try {
        Emitter emitter{scope, layout, target, reuse, {&block, 1}, {}, 0, true, profile};
        emitter.Begin(0);
        bool ended{};
        u32 position{};
        while (true) {
            std::array<u8, 4> edge;
            const auto bytes = InstructionBytes(windows, Location{pc}.Address(), edge);
            if (bytes.empty()) {
                if (!position) return {BuildError::Truncated, pc};
                break;
            }
            const auto decoded = DecodeOne(bytes, pc, timing);
            if (!decoded) {
                if (position) break;
                return {decoded.error == DecodeError::Truncated ? BuildError::Truncated
                                                                : BuildError::InvalidEncoding,
                        pc};
            }
            ended = emitter.Instruction(decoded.instruction, position++);
            Continuation(decoded.instruction, continuations);
            pc = decoded.instruction.NextPC();
            if (ended || decoded.instruction.Boundary()) break;
        }
        emitter.Finish(ended);
        if (sources)
            sources->push_back(
                    {Location{entry}.Address(), emitter.physical_end - Location{entry}.Address()});
        return emitter.Submit(counts);
    } catch (const detail::Rejected& failure) {
        return {failure.error, pc, 0, 0, failure.detail, {}};
    }
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
                    std::vector<u64>* continuations,
                    Profile profile) {
    return BuildIR(windows,
                   windows.empty() ? 0 : windows.front().pc,
                   scope,
                   layout,
                   target,
                   counts,
                   timing,
                   external,
                   reuse,
                   sources,
                   continuations,
                   profile);
}
}  // namespace swift::arm32
