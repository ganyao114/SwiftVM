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

struct InlineExitList {
    std::array<u64, 64> local{};
    std::size_t local_count{};
    std::vector<u64> heap;

    void Push(u64 addr) {
        if (heap.empty() && local_count < local.size()) {
            local[local_count++] = addr;
            return;
        }
        if (heap.empty()) {
            heap.reserve(local.size() * 2);
            heap.assign(local.begin(), local.begin() + local_count);
        }
        heap.push_back(addr);
    }

    std::span<const u64> FinalizeSortedUnique() {
        u64* begin = heap.empty() ? local.data() : heap.data();
        std::size_t size = heap.empty() ? local_count : heap.size();
        std::sort(begin, begin + size);
        size = static_cast<std::size_t>(std::unique(begin, begin + size) - begin);
        if (heap.empty())
            local_count = size;
        else
            heap.resize(size);
        return {begin, size};
    }
};

BlockId LookupInternalBlock(std::span<const DecodedBlock> blocks,
                            Scope scope,
                            bool external,
                            u64 address) {
    if (external || scope == Scope::Block) return BlockId{};
    const auto i = std::lower_bound(
            blocks.begin(), blocks.end(), address, [](const auto& b, u64 p) { return b.pc < p; });
    return i != blocks.end() && i->pc == address && i->instruction_count
                   ? BlockId{u32(i - blocks.begin())}
                   : BlockId{};
}

InlineExitList CollectExternalExits(std::span<const Instruction> instructions,
                                    std::span<const DecodedBlock> blocks,
                                    Scope scope,
                                    bool external) {
    InlineExitList exits;
    if (scope != Scope::Function || external) return exits;
    for (const auto& b : blocks) {
        if (!b.instruction_count) continue;
        const auto& last = instructions[b.first_instruction + b.instruction_count - 1];
        if (last.Flow() != FlowKind::Conditional) continue;
        u64 taken{};
        if (last.RelativeTarget(taken)) {
            if (!LookupInternalBlock(blocks, scope, external, taken)) exits.Push(taken);
            if (!LookupInternalBlock(blocks, scope, external, last.PC() + 4))
                exits.Push(last.PC() + 4);
        }
    }
    return exits;
}

struct DecodedEmitter {
    using OwnerId = decltype(std::declval<detail::IRSink>().UnitId());

    detail::IRSink& sink;
    std::span<const DecodedBlock> blocks;
    std::span<const u64> exits;
    Scope scope;
    StateLayout layout;
    TargetInfo target;
    OwnerId owner;
    bool external;

    BlockId Internal(u64 address) const {
        return LookupInternalBlock(blocks, scope, external, address);
    }

    BlockRef TargetBlock(u64 address) const {
        if (const auto id = Internal(address)) return BlockRef{owner, id};
        const auto i = std::lower_bound(exits.begin(), exits.end(), address);
        return BlockRef{owner, BlockId{u32(blocks.size() + std::size_t(i - exits.begin()))}};
    }

    void StorePC(Value value) {
        Require(sink.Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(layout.pc_offset, Type::U64), Operand::ValueOf(value)}));
    }

    void DispatchTo(u64 address) {
        StorePC(Constant(sink, address));
        Require(sink.EndBlock(Terminal::Dispatch()));
    }

    void JumpOrDispatch(u64 address) {
        if (const auto id = Internal(address))
            Require(sink.EndBlock(Terminal::Jump({owner, id})));
        else
            DispatchTo(address);
    }

    bool EmitControl(const Sem::Control& control) {
        using K = Sem::Control::Kind;
        switch (control.kind) {
            case K::None:
                return false;
            case K::Jump:
                JumpOrDispatch(control.yes);
                return true;
            case K::Conditional:
                if (scope == Scope::Block || external) {
                    if (target.Has(TargetFeature::ConditionalUniformStore)) {
                        Require(sink.Emit<OpCode::StoreUniformSelectImm>(
                                Type::Void,
                                {Operand::Uniform(layout.pc_offset, Type::U64),
                                 Operand::ValueOf(control.condition.raw, control.condition.view),
                                 Operand::Immediate(control.yes, Type::U64),
                                 Operand::Immediate(control.no, Type::U64)}));
                    } else {
                        const auto yes = Constant(sink, control.yes);
                        const auto no = Constant(sink, control.no);
                        StorePC(Require(sink.Emit<OpCode::Select>(
                                Type::U64,
                                {Operand::ValueOf(control.condition.raw),
                                 Operand::ValueOf(yes),
                                 Operand::ValueOf(no)})));
                    }
                    Require(sink.EndBlock(Terminal::Dispatch()));
                } else {
                    Require(sink.EndBlock(Terminal::Branch(control.condition.raw,
                                                           TargetBlock(control.yes),
                                                           TargetBlock(control.no))));
                }
                return true;
            case K::Dispatch:
                StorePC(control.destination.IsImm() ? Constant(sink, control.destination.imm.bits)
                                                    : control.destination.value.raw);
                Require(sink.EndBlock(Terminal::Dispatch()));
                return true;
            case K::Service:
                StorePC(Constant(sink, control.yes));
                Require(sink.EndBlock(Terminal::Return(Constant(sink, control.service))));
                return true;
        }
        return false;
    }

    void EmitBlock(u32 index,
                   std::span<const Instruction> instructions,
                   u32 entry,
                   Sem& semantics,
                   u64& pc,
                   std::size_t& total,
                   std::size_t& bytes,
                   std::vector<runtime::SourceSpan>* sources,
                   std::vector<u64>* continuations) {
        const auto& b = blocks[index];
        pc = b.pc;
        if (b.first_instruction > instructions.size() ||
            b.instruction_count > instructions.size() - b.first_instruction)
            throw detail::Rejected{BuildError::InvalidInput};
        Require(sink.BeginBlock({owner, BlockId{index}}));
        if (scope == Scope::Function && index != entry && b.instruction_count &&
            target.Has(TargetFeature::ExternalEntries))
            Require(sink.Emit<OpCode::EntryPoint>(Type::Void, {Operand::Immediate(pc, Type::U64)}));
        bool ended{};
        for (u32 j = 0; j < b.instruction_count; ++j) {
            const auto& inst = instructions[b.first_instruction + j];
            if (inst.PC() != pc) throw detail::Rejected{BuildError::InvalidInput};
            semantics.Execute(inst, j && j % runtime::frontend::GuestEventInstructionBudget == 0);
            ++total;
            bytes += 4;
            pc += 4;
            if (continuations &&
                (inst.Flow() == FlowKind::Call || inst.Flow() == FlowKind::Service ||
                 (inst.Boundary() && inst.Flow() == FlowKind::Linear)))
                continuations->push_back(pc);
            ended = EmitControl(semantics.control);
            if (ended) {
                if (j + 1 != b.instruction_count) throw detail::Rejected{BuildError::InvalidInput};
                break;
            }
        }
        if (!ended) JumpOrDispatch(pc);
        if (sources && pc > b.pc) sources->push_back({b.pc, pc - b.pc});
    }
};

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
        auto exit_storage = CollectExternalExits(instructions, blocks, scope, external);
        const auto exits = exit_storage.FinalizeSortedUnique();
        for (std::size_t i = 0; i < blocks.size() + exits.size(); ++i) {
            const auto r = sink.CreateBlock();
            Require(r.error);
        }
        const auto owner = sink.UnitId();
        Require(sink.SetEntry({owner, BlockId{entry}}));
        DecodedEmitter emitter{sink, blocks, exits, scope, layout, target, owner, external};
        Sem semantics{sink, layout, target};
        for (u32 n = 0; n < blocks.size(); ++n) {
            emitter.EmitBlock(
                    n, instructions, entry, semantics, pc, total, bytes, sources, continuations);
        }
        for (std::size_t i = 0; i < exits.size(); ++i) {
            Require(sink.BeginBlock({owner, BlockId{u32(blocks.size() + i)}}));
            emitter.DispatchTo(exits[i]);
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
