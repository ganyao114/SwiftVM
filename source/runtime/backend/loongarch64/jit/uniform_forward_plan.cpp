#include "uniform_forward_plan.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include "sparse_homes_plan.h"

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
u32 Bytes(T type) { return type >= T::U8 && type <= T::V256 ? ir::GetValueSizeByte(type) : 0; }
bool Range(u32 offset, u32 size, std::optional<u32> extent) {
    return size && offset <= UINT32_MAX - size &&
           (!extent || (offset <= *extent && size <= *extent - offset));
}
bool Branch(ir::Inst* inst) { return inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto; }

bool Barrier(ir::Inst* inst) {
    // Positive inventory. Native memory still calls the external range oracle;
    // callbacks, unknown instructions and escaped State addresses kill facts.
    // Poll/AdvancePC continuations only observe State and never change uniforms.
    switch (inst->GetOp()) {
        case O::LoadUniform: case O::StoreUniform: return false;
        case O::LoadImm: case O::Zero: case O::BitCast: case O::GetOperand: case O::GetResult:
        case O::Select: case O::SelectZero: case O::CondSelect:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb: case O::Mul: case O::MulSub:
        case O::MulHigh: case O::Div: case O::SignedDiv64:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::And: case O::Or: case O::Xor: case O::Not: case O::AndNot: case O::Neg:
        case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
        case O::LslValue: case O::LsrValue: case O::AsrValue: case O::RorValue:
        case O::ByteSwap: case O::CountLeadingZeros64: case O::CountLeadingZeros32:
        case O::CountTrailingZeros64: case O::CountTrailingZeros32: case O::PopCount:
        case O::BitExtract: case O::BitInsert: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
        case O::Crc32: case O::Crc32c:
            return SparseHomesPlan::MayCallTypedHelper(inst);
        case O::GetFlags: case O::SaveFlags: case O::BranchOnlyFlags:
        case O::TestFlags: case O::TestNotFlags: case O::ClearFlags:
        case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
        case O::CondSet: case O::LocalCondSet: case O::LocalParitySet:
        case O::Goto: case O::NotGoto: case O::BindLabel: case O::AddPhi:
        case O::SetLocation: case O::GetLocation: case O::AdvancePC: case O::Checkpoint:
        case O::Nop: case O::XchgBarrier: case O::PushRSB: case O::PopRSB: case O::CallReturn:
            return false;
        default: return true;
    }
}
struct Fact {
    ir::Value value;
    u8 byte{};
    bool operator==(const Fact& other) const {
        return value == other.value && byte == other.byte &&
               (!value.Defined() || value.Type() == other.value.Type());
    }
};
using Facts = std::vector<Fact>;
struct Node {
    u32 first{}, end{};
    std::vector<u32> incoming, outgoing;
    bool reachable{}, initialized{};
    Facts in, out;
};
}  // namespace

UniformForwardPlan UniformForwardPlan::Build(ir::Block* block, const RegisterPlan::Targets& targets,
                                           const RegisterPlan::Stops& stops,
                                           const RegisterPlan::Exits& exits,
                                           std::optional<u32> extent) {
    if (!block) throw std::invalid_argument("LoongArch uniform forwarding needs a block");
    UniformForwardPlan result;
    result.stats.enabled = true;
    std::vector<ir::Inst*> instructions;
    std::unordered_map<ir::Inst*, u32> positions, labels;
    std::vector<u32> bytes;
    for (auto& inst : block->GetInstList()) {
        positions.emplace(&inst, instructions.size()); instructions.push_back(&inst);
        if (inst.GetOp() == O::BindLabel && !labels.emplace(inst.GetArg<ir::Value>(0).Def(), positions.at(&inst)).second)
            throw std::runtime_error("LoongArch uniform forwarding has a duplicate label");
        if (inst.GetOp() == O::LoadUniform || inst.GetOp() == O::StoreUniform) {
            const auto offset = inst.GetArg<ir::Uniform>(0).GetOffset();
            const auto size = Bytes(inst.GetOp() == O::LoadUniform ? inst.ReturnType() : inst.GetArg<ir::Value>(1).Type());
            if (Range(offset, size, extent)) for (u32 byte = 0; byte < size; ++byte) bytes.push_back(offset + byte);
            if (inst.GetOp() == O::LoadUniform) ++result.stats.loads_seen;
            else ++result.stats.stores_retained;
        }
        if (Barrier(&inst)) ++result.stats.barriers;
    }
    if (instructions.empty()) { result.stats.accepted = true; return result; }
    std::sort(bytes.begin(), bytes.end()); bytes.erase(std::unique(bytes.begin(), bytes.end()), bytes.end());
    const auto index = [&](u32 offset) { return u32(std::lower_bound(bytes.begin(), bytes.end(), offset) - bytes.begin()); };
    const auto position = [&](ir::Inst* inst) {
        const auto found = positions.find(inst);
        if (found == positions.end()) throw std::runtime_error("LoongArch uniform forwarding has a foreign CFG instruction");
        return found->second;
    };
    const auto stopped = [&](ir::Inst* inst) { return stops.contains(inst) || exits.contains(inst); };
    std::vector<bool> leaders(instructions.size() + 1);
    leaders.front() = leaders.back() = true;
    std::unordered_map<ir::Inst*, u32> branch_targets;
    for (u32 i = 0; i < instructions.size(); ++i) {
        auto* inst = instructions[i];
        if (inst->GetOp() == O::BindLabel) leaders[i] = true;
        if (Branch(inst) || stopped(inst)) leaders[i + 1] = true;
        if (Branch(inst)) {
            auto* anchor = targets.contains(inst) ? targets.at(inst) : inst;
            auto found = labels.find(anchor);
            const auto destination = found != labels.end() ? found->second :
                anchor && anchor->GetOp() == O::BindLabel ? position(anchor) : UINT32_MAX;
            if (destination == UINT32_MAX) throw std::runtime_error("LoongArch uniform forwarding has an unbound branch");
            branch_targets.emplace(inst, destination); leaders[destination] = true;
        }
    }
    std::vector<Node> nodes;
    std::vector<u32> node_at(instructions.size());
    for (u32 i = 0; i < instructions.size(); ++i) {
        if (leaders[i]) {
            if (!nodes.empty()) nodes.back().end = i;
            nodes.emplace_back(); nodes.back().first = i;
        }
        node_at[i] = nodes.size() - 1;
    }
    nodes.back().end = instructions.size(); result.stats.cfg_nodes = nodes.size();
    // Bound cold-path memory and dominance work; rejection keeps the IR intact.
    constexpr u32 max_cells = 1u << 20;
    if (nodes.size() > 2048 || (!bytes.empty() && nodes.size() > max_cells / bytes.size())) return result;
    const auto edge = [&](u32 from, u32 to) {
        nodes[from].outgoing.push_back(to); nodes[to].incoming.push_back(from);
    };
    for (u32 i = 0; i < nodes.size(); ++i) {
        auto* last = instructions[nodes[i].end - 1];
        if (!stopped(last) && i + 1 < nodes.size()) edge(i, i + 1);
        if (Branch(last)) edge(i, node_at[branch_targets.at(last)]);
    }
    std::vector<u32> work{0}; nodes.front().reachable = true;
    for (u32 at = 0; at < work.size(); ++at) for (auto next : nodes[work[at]].outgoing)
        if (!nodes[next].reachable) { nodes[next].reachable = true; work.push_back(next); }
    const u32 words = (nodes.size() + 63) / 64;
    std::vector<u64> all(words);
    for (u32 i = 0; i < nodes.size(); ++i) if (nodes[i].reachable) all[i / 64] |= u64{1} << (i % 64);
    std::vector<std::vector<u64>> dominators(nodes.size(), all);
    std::fill(dominators.front().begin(), dominators.front().end(), 0); dominators.front()[0] = 1;
    bool changed;
    do {
        changed = false;
        for (u32 i = 1; i < nodes.size(); ++i) if (nodes[i].reachable) {
            auto next = all;
            for (auto from : nodes[i].incoming) if (nodes[from].reachable)
                for (u32 word = 0; word < words; ++word) next[word] &= dominators[from][word];
            next[i / 64] |= u64{1} << (i % 64);
            if (next != dominators[i]) { dominators[i] = std::move(next); changed = true; }
        }
    } while (changed);
    const auto dominates = [&](ir::Inst* source, u32 use) {
        const auto definition = position(source), from = node_at[definition], to = node_at[use];
        return (dominators[to][from / 64] & (u64{1} << (from % 64))) && (from != to || definition < use);
    };
    const auto transfer = [&](ir::Inst* inst, Facts& facts) {
        // An SSA definition in a loop executes again; its static pointer is
        // not the previous iteration's snapshot. Phi destinations are also
        // assigned on every incoming edge. Never keep a uniform alias of an
        // old dynamic definition after that instruction has produced a new one.
        if (inst->ReturnType() != T::VOID)
            for (auto& fact : facts) if (fact.value.Def() == inst) fact = {};
        if (Barrier(inst)) { std::fill(facts.begin(), facts.end(), Fact{}); return; }
        if (inst->GetOp() != O::LoadUniform && inst->GetOp() != O::StoreUniform) return;
        const auto offset = inst->GetArg<ir::Uniform>(0).GetOffset();
        const bool store = inst->GetOp() == O::StoreUniform;
        const auto value = store ? inst->GetArg<ir::Value>(1) : ir::Value{inst};
        const auto size = Bytes(value.Type());
        if (!Range(offset, size, extent) || size > 16) {
            std::fill(facts.begin(), facts.end(), Fact{}); return;
        }
        if (!store && !Scalar(inst->ReturnType())) return;
        for (u32 byte = 0; byte < size; ++byte) {
            auto& fact = facts[index(offset + byte)];
            if (store) fact = Scalar(value.Type()) && Scalar(value.Def()->ReturnType()) ? Fact{value, u8(byte)} : Fact{};
            // A redundant load must not replace a dominating snapshot with a
            // branch-local identity, which would lose a valid fact at the join.
            else if (!fact.value.Defined()) fact = {value, u8(byte)};
        }
    };
    for (auto& node : nodes) { node.in.resize(bytes.size()); node.out.resize(bytes.size()); }
    do {
        changed = false;
        if (++result.stats.iterations > 1024) return result;
        for (u32 i = 0; i < nodes.size(); ++i) if (nodes[i].reachable) {
            Facts incoming(bytes.size()); bool ready = i == 0;
            for (auto from : nodes[i].incoming) if (nodes[from].initialized && nodes[from].reachable) {
                if (!ready) { incoming = nodes[from].out; ready = true; }
                else for (u32 byte = 0; byte < bytes.size(); ++byte)
                    if (!(incoming[byte] == nodes[from].out[byte])) incoming[byte] = {};
            }
            if (!ready) continue;
            auto outgoing = incoming;
            for (u32 at = nodes[i].first; at < nodes[i].end; ++at) transfer(instructions[at], outgoing);
            if (!nodes[i].initialized || incoming != nodes[i].in || outgoing != nodes[i].out) {
                nodes[i].in = std::move(incoming); nodes[i].out = std::move(outgoing);
                nodes[i].initialized = true; changed = true;
            }
        }
    } while (changed);
    for (const auto& node : nodes) if (node.reachable) {
        if (node.incoming.size() > 1) ++result.stats.joins;
        auto facts = node.in;
        for (u32 at = node.first; at < node.end; ++at) {
            auto* inst = instructions[at];
            if (inst->GetOp() == O::LoadUniform && Scalar(inst->ReturnType())) {
                const auto uniform = inst->GetArg<ir::Uniform>(0);
                const auto size = Bytes(inst->ReturnType()), offset = uniform.GetOffset();
                if (Range(offset, size, extent)) {
                    const auto source = facts[index(offset)];
                    bool complete = source.value.Defined();
                    for (u32 byte = 0; complete && byte < size; ++byte) {
                        const auto& next = facts[index(offset + byte)];
                        complete = next.value == source.value && next.value.Type() == source.value.Type() && next.byte == source.byte + byte;
                    }
                    if (complete) {
                        if (Scalar(source.value.Type()) && Scalar(source.value.Def()->ReturnType()) &&
                            u32(source.byte) + size <= Bytes(source.value.Type()) && dominates(source.value.Def(), at)) {
                            result.replacements.push_back({inst, source.value, source.byte, size, uniform, inst->ReturnType(), false});
                            ++result.stats.candidates;
                        } else ++result.stats.rejected_sources;
                    }
                }
            }
            transfer(inst, facts);
        }
    }
    result.stats.accepted = true;
    return result;
}

void UniformForwardPlan::Apply() {
    for (auto& replacement : replacements) if (!replacement.active) {
        replacement.load->Reset();
        replacement.load->SetInst(O::BitExtract, replacement.source, ir::Imm{replacement.first_byte * 8}, ir::Imm{replacement.size * 8});
        replacement.load->SetReturn(replacement.type);
        replacement.active = true; ++stats.loads_forwarded;
    }
}
void UniformForwardPlan::Restore(Replacement& replacement) {
    replacement.load->Reset(); replacement.load->SetInst(O::LoadUniform, replacement.uniform);
    replacement.load->SetReturn(replacement.type); replacement.active = false; --stats.loads_forwarded;
}
void UniformForwardPlan::RejectAll() {
    for (auto& replacement : replacements) if (replacement.active) { Restore(replacement); ++stats.allocation_rejections; }
}
bool UniformForwardPlan::RejectSpillGrowth(u32 spills) {
    RecordAllocatedSpills(spills);
    if (!stats.spill_audit || spills <= stats.baseline_spilled_values || !stats.loads_forwarded) return false;
    // This rare pressure fallback deliberately keeps the exact OFF block.
    // There is no profitable subset proven by the current coloring.
    stats.spill_growth_rejections += stats.loads_forwarded;
    RejectAll();
    return true;
}

}  // namespace swift::runtime::backend::loongarch64
