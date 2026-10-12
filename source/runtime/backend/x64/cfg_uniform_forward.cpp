#include "cfg_uniform_forward.h"
#include "translator.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
struct Fact {
    ir::Value value;
    u8 byte{};
    bool operator==(const Fact& other) const {
        return value == other.value && byte == other.byte &&
               (!value.Defined() || value.Type() == other.value.Type());
    }
};
using Facts = std::vector<Fact>;
bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
u32 Width(T type) { return type >= T::U8 && type <= T::V128 ? ir::GetValueSizeByte(type) : 0; }
bool Range(u32 offset, u32 size, u32 extent) {
    return size && offset <= extent && size <= extent - offset;
}
bool Barrier(O op) {
    // Positive inventory. New opcodes cannot accidentally authorize reuse
    // across a callback, fault, State-pointer escape, or uniform mutation.
    switch (op) {
        case O::LoadUniform: case O::StoreUniform: case O::LoadImm:
        case O::Nop: case O::AdvancePC: case O::Zero:
        case O::BitCast: case O::GetOperand: case O::GetResult:
        case O::Select: case O::SelectZero: case O::CondSelect:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::Mul: case O::MulSub: case O::MulHigh:
        case O::Div: case O::SignedDiv64: case O::Div128: case O::Div128Remainder:
        case O::CpuidUpper:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::And: case O::Or: case O::Xor: case O::Not: case O::AndNot:
        case O::GetFlags: case O::SaveFlags: case O::BranchOnlyFlags:
        case O::TestFlags: case O::TestNotFlags: case O::ClearFlags:
        case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue:
        case O::ByteSwap: case O::CountLeadingZeros64: case O::CountLeadingZeros32:
        case O::CountTrailingZeros64: case O::CountTrailingZeros32:
        case O::BitExtract: case O::BitInsert: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
        case O::VecXor: case O::VecAnd: case O::VecOr: case O::VecAndNot:
        case O::CondSet:
        // These affect PC, flags or SSA edge transport, not uniform bytes.
        // All stores remain committed before an interrupt/fault exit.
        case O::Goto: case O::NotGoto: case O::BindLabel: case O::AddPhi:
        case O::SetLocation: case O::GetLocation:
            return false;
        default: return true;
    }
}
void Remember(Facts& facts, u32 offset, u32 size, ir::Value value) {
    for (u32 byte = 0; byte < size; ++byte) facts[offset + byte] = {value, u8(byte)};
}
void Transfer(ir::Inst* inst, Facts& facts, u32 extent) {
    if (Barrier(inst->GetOp())) {
        std::fill(facts.begin(), facts.end(), Fact{});
    } else if (inst->GetOp() == O::StoreUniform) {
        const auto uniform = inst->GetArg<ir::Uniform>(0);
        const auto value = inst->GetArg<ir::Value>(1);
        const auto size = Width(value.Type());
        if (!value.Defined() || !Range(uniform.GetOffset(), size, extent))
            std::fill(facts.begin(), facts.end(), Fact{});
        else Remember(facts, uniform.GetOffset(), size, value);
    } else if (inst->GetOp() == O::LoadUniform) {
        if (inst->ArgAt(0).GetType() != ir::ArgType::Uniform) {
            std::fill(facts.begin(), facts.end(), Fact{}); return;
        }
        const auto uniform = inst->GetArg<ir::Uniform>(0);
        const auto type = inst->ReturnType();
        const auto size = Width(type);
        // Narrow vector loads initialize their high bytes. Do not interpret
        // such a producer as an interchangeable raw scalar/vector container.
        if (type == uniform.GetType() && (Scalar(type) || type == T::V128) &&
            Range(uniform.GetOffset(), size, extent)) {
            // Reading does not change any uniform byte. Keep an earlier
            // proved identity rather than replacing it with a redundant
            // branch-local load: otherwise the join would meet old/new
            // identities as unknown even when this load will be forwarded.
            // Only unknown bytes gain this load's current snapshot identity.
            for (u32 byte = 0; byte < size; ++byte) {
                auto& fact = facts[uniform.GetOffset() + byte];
                if (!fact.value.Defined()) fact = {ir::Value{inst}, u8(byte)};
            }
        }
    }
}
struct Replacement {
    ir::Inst* load;
    ir::Value source;
    u32 byte, size;
    T type;
    ir::Uniform original;
    u16 id;
    bool active{true};
};
CfgUniformForwardStats Forward(backend::FunctionPlan& function,
                                             u32 extent,
                                             const CfgRegisterPlan& graph, CfgUniformForwardOptions options,
                                             std::optional<CfgRegisterPlan>* final_allocation) {
    if (final_allocation) final_allocation->reset();
    CfgUniformForwardStats stats;
    stats.allocation_policy_enabled = options.require_scalar_source_register;
    auto* body = function.body.get();
    if (!body || !extent || !graph.Enabled() || !graph.MatchesBody(body)) return stats;
    const auto& instructions = graph.Instructions();
    const auto& blocks = graph.Blocks();
    const auto& edges = graph.Edges();
    // Bound cold-path analysis storage; rejection does not change the body.
    constexpr size_t max_cells = 1u << 20;
    if (blocks.empty() || extent > max_cells / blocks.size()) return stats;
    std::vector<Facts> incoming(blocks.size(), Facts(extent));
    std::vector<Facts> outgoing(blocks.size(), Facts(extent));
    Facts scratch(extent);
    bool changed{};
    do {
        changed = false;
        ++stats.iterations;
        // No Phi synthesis/bootstrap: unknown inputs stay unknown until every
        // reachable predecessor proves the same fact. Loop headers therefore
        // cannot infer a value only from their own initially empty backedge.
        for (u32 index = 0; index < blocks.size(); ++index) {
            const auto& block = blocks[index];
            if (!block.reachable) continue;
            std::fill(scratch.begin(), scratch.end(), Fact{});
            bool first = true;
            for (const auto edge_index : block.incoming_edges) {
                const auto& edge = edges.at(edge_index);
                if (!edge.host_entry && !blocks.at(edge.from).reachable) continue;
                if (edge.host_entry) {
                    std::fill(scratch.begin(), scratch.end(), Fact{});
                    first = false;
                    break;
                }
                const auto& predecessor = outgoing.at(edge.from);
                if (first) { scratch = predecessor; first = false; }
                else for (u32 byte = 0; byte < extent; ++byte)
                    if (!(scratch[byte] == predecessor[byte])) scratch[byte] = {};
            }
            if (incoming[index] != scratch) { incoming[index] = scratch; changed = true; }
            for (u32 position = block.begin; position < block.end; ++position)
                Transfer(instructions.at(position), scratch, extent);
            if (outgoing[index] != scratch) { outgoing[index] = scratch; changed = true; }
        }
        // Conservative fallback if an unexpected graph exceeds convergence
        // budget. Nothing has been rewritten yet.
        if (changed && stats.iterations > blocks.size() + 1) return stats;
    } while (changed);

    std::unordered_map<ir::Inst*, std::pair<u32, u32>> positions;
    for (u32 index = 0; index < blocks.size(); ++index)
        for (u32 position = blocks[index].begin; position < blocks[index].end; ++position)
            positions.emplace(instructions.at(position), std::pair{index, position});
    std::vector<Replacement> replacements;
    for (u32 index = 0; index < blocks.size(); ++index) {
        const auto& block = blocks[index];
        if (!block.reachable) continue;
        scratch = incoming[index];
        for (u32 position = block.begin; position < block.end; ++position) {
            auto* inst = instructions.at(position);
            if (inst->GetOp() == O::StoreUniform) ++stats.stores_retained;
            if (Barrier(inst->GetOp())) ++stats.barriers;
            if (inst->GetOp() == O::LoadUniform) {
                ++stats.loads_seen;
                if (inst->ArgAt(0).GetType() != ir::ArgType::Uniform) return stats;
                const auto uniform = inst->GetArg<ir::Uniform>(0);
                const auto type = inst->ReturnType();
                const auto size = Width(type);
                if (type == uniform.GetType() && (Scalar(type) || type == T::V128) &&
                    Range(uniform.GetOffset(), size, extent)) {
                    // Rewrite only plain loads. Extra physical inputs still
                    // participate in facts/SSA validation, but Reset must not
                    // destroy them on either the normal or rollback path.
                    bool plain = true;
                    for (u32 slot = 1; slot < ir::Inst::max_args; ++slot) plain &= inst->ArgAt(slot).IsVoid();
                    const auto fact = scratch[uniform.GetOffset()];
                    bool complete = fact.value.Defined();
                    for (u32 byte = 0; complete && byte < size; ++byte) {
                        const auto& other = scratch[uniform.GetOffset() + byte];
                        complete = other.value == fact.value && other.byte == fact.byte + byte &&
                                   other.value.Type() == fact.value.Type();
                    }
                    const auto source_position = positions.find(fact.value.Def());
                    complete &= source_position != positions.end();
                    if (complete) {
                        const auto [source_block, source_order] = source_position->second;
                        complete = source_block == index ? source_order < position
                                                         : graph.DominatesBlock(source_block, index);
                    }
                    if (complete && plain) {
                        const auto actual = fact.value.Def()->ReturnType();
                        const auto source_size = Width(fact.value.Type());
                        if (Scalar(type) && Scalar(fact.value.Type()) && Scalar(actual) &&
                            fact.byte <= source_size && size <= source_size - fact.byte)
                            replacements.push_back({inst, fact.value, fact.byte, size, type, uniform, inst->Id()});
                        else if (type == T::V128 && fact.value.Type() == T::V128 &&
                                 actual == T::V128 && fact.byte == 0)
                            replacements.push_back({inst, fact.value, 0, size, type, uniform, inst->Id()});
                    }
                }
            }
            // Unknown bytes acquire the original load's current snapshot;
            // known facts survive an equivalent, possibly folded, read.
            Transfer(inst, scratch, extent);
        }
    }
    stats.candidate_loads = replacements.size();
    stats.scalar_candidates = std::count_if(replacements.begin(), replacements.end(),
                                            [](const auto& replacement) { return Scalar(replacement.type); });
    const auto restore = [](Replacement& replacement) {
        replacement.load->Reset();
        replacement.load->LoadUniform(replacement.original).SetReturn(replacement.type);
        replacement.active = false;
    };
    try {
        for (const auto& replacement : replacements) {
            replacement.load->Reset();
            if (Scalar(replacement.type))
                replacement.load->BitExtract(replacement.source, ir::Imm{replacement.byte * 8},
                                             ir::Imm{replacement.size * 8}).SetReturn(replacement.type);
            else replacement.load->BitCast(replacement.source).SetReturn(replacement.type);
        }
        if (options.require_scalar_source_register && !replacements.empty()) {
            constexpr u32 max_rounds = 4;
            for (u32 round = 0; round < max_rounds; ++round) {
                ++stats.allocation_rounds;
                auto allocation = CfgRegisterPlan::Build(body, function, options.allocation_budget);
                if (!allocation.Enabled() || !allocation.Validate()) throw std::runtime_error("forward allocation rejected");
                bool dropped = false;
                for (auto& replacement : replacements)
                    if (replacement.active && Scalar(replacement.type) && !allocation.Register(replacement.source.Def())) {
                        restore(replacement); ++stats.rejected_scalar_sources; dropped = true;
                    }
                if (!dropped) {
                    stats.final_scalar_sources_allocated = true;
                    if (final_allocation) *final_allocation = std::move(allocation);
                    break;
                }
                if (round + 1 == max_rounds) {
                    stats.allocation_limit_reached = true;
                    for (auto& replacement : replacements) if (replacement.active && Scalar(replacement.type)) {
                        restore(replacement); ++stats.rejected_scalar_sources;
                    }
                    // Vector rewrites remain after this conservative scalar
                    // fallback. Validate this final body, never a stale plan.
                    if (!CfgRegisterPlan::ValidateGraph(body, function).Enabled())
                        throw std::runtime_error("forward fallback graph rejected");
                    stats.final_scalar_sources_allocated = true; // No scalar candidates remain.
                }
            }
        } else if (options.require_scalar_source_register) stats.final_scalar_sources_allocated = true;
        for (const auto& replacement : replacements)
            if (replacement.load->Id() != replacement.id) throw std::runtime_error("forward changed instruction ID");
    } catch (...) {
        if (final_allocation) final_allocation->reset();
        for (auto& replacement : replacements) restore(replacement);
        stats.final_scalar_sources_allocated = false;
        return stats;
    }
    for (const auto& replacement : replacements) if (replacement.active) {
        if (Scalar(replacement.type)) ++stats.scalar_loads; else ++stats.vector_loads;
        ++stats.loads_forwarded;
    }
    stats.accepted = true;
    return stats;
}
} // namespace

CfgUniformForwardStats ForwardCfgUniformLoads(backend::FunctionPlan& function, u32 extent,
                                              const CfgRegisterPlan& graph, CfgUniformForwardOptions options) {
    return Forward(function, extent, graph, std::move(options), nullptr);
}

CfgUniformForwardStats Translator::ForwardForPreparation(backend::FunctionPlan& function, u32 extent,
                                              const CfgRegisterPlan& graph, CfgUniformForwardOptions options,
                                              std::optional<CfgRegisterPlan>& allocation) {
    return Forward(function, extent, graph, std::move(options), &allocation);
}
} // namespace swift::runtime::backend::x64
