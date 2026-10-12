#include "cfg_state_promote.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
u32 Width(T type) {
    return type >= T::U8 && type <= T::V128 ? ir::GetValueSizeByte(type) : 0;
}
bool Range(u32 offset, u32 bytes, u32 extent) {
    return bytes && offset <= extent && bytes <= extent - offset;
}
bool Overlap(u32 a, u32 bytes, u32 lane) {
    return u64(a) < u64(lane) + 8 && u64(lane) < u64(a) + bytes;
}
bool Barrier(O op) {
    // Positive inventory: callbacks, guest memory, state pointer escape and
    // new opcodes are conservative barriers even if their IR returns a value.
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
        case O::Goto: case O::NotGoto: case O::BindLabel: case O::AddPhi:
        case O::SetLocation: case O::GetLocation:
            return false;
        default: return true;
    }
}
struct Access {
    bool barrier{}, load{}, store{}, full_load{}, full_store{}, partial_store{};
    u32 byte{}, bytes{};
    ir::Value stored;
};
Access Classify(ir::Inst* inst, u32 lane, u32 extent) {
    if (Barrier(inst->GetOp())) return {.barrier = true};
    if (inst->GetOp() == O::LoadUniform) {
        if (inst->ArgAt(0).GetType() != ir::ArgType::Uniform)
            return {.barrier = true};
        const auto uniform = inst->GetArg<ir::Uniform>(0);
        const auto type = inst->ReturnType();
        const auto bytes = Width(type);
        if (!Range(uniform.GetOffset(), bytes, extent)) return {.barrier = true};
        // Uniform reads never invalidate state, including vector/raw views.
        if (!Scalar(type) || type != uniform.GetType() ||
            uniform.GetOffset() < lane || uniform.GetOffset() - lane > 8 ||
            bytes > 8 - (uniform.GetOffset() - lane)) return {};
        // Only plain loads can be rewritten/rolled back without extra args.
        for (u32 slot = 1; slot < ir::Inst::max_args; ++slot)
            if (!inst->ArgAt(slot).IsVoid()) return {};
        return {.load = true, .full_load = bytes == 8,
                .byte = uniform.GetOffset() - lane, .bytes = bytes};
    }
    if (inst->GetOp() == O::StoreUniform) {
        if (inst->ArgAt(0).GetType() != ir::ArgType::Uniform || !inst->ArgAt(1).IsValue())
            return {.barrier = true};
        const auto uniform = inst->GetArg<ir::Uniform>(0);
        const auto value = inst->GetArg<ir::Value>(1);
        const auto bytes = Width(value.Type());
        if (!value.Defined() || !Range(uniform.GetOffset(), bytes, extent))
            return {.barrier = true};
        if (!Overlap(uniform.GetOffset(), bytes, lane)) return {};
        const bool full = uniform.GetOffset() == lane && bytes == 8 &&
                          Scalar(value.Type()) && Scalar(value.Def()->ReturnType());
        return {.store = true, .full_store = full, .partial_store = !full,
                .bytes = bytes, .stored = value};
    }
    return {};
}
using Bits = std::vector<u8>;
struct Replacement {
    ir::Inst* load{};
    ir::Value source;
    ir::Uniform uniform;
    T type{};
    u32 byte{}, bytes{}, lane{};
};
struct Phi {
    ir::Inst* inst{};
    u32 block{}, lane{};
    std::vector<ir::Value> inputs; // Undefined means unreachable structural edge.
};
struct PendingInstructions {
    std::vector<std::unique_ptr<ir::Inst>> nodes;
    std::vector<ir::Inst*> inserted;
    ~PendingInstructions() {
        // First unregister every cyclic Phi argument while all nodes exist.
        for (auto& node : nodes) if (node) node->DestroyArgs();
    }
    ir::Inst* New(O op) {
        auto owner = std::make_unique<ir::Inst>(op);
        auto* result = owner.get();
        result->SetReturn(T::U64);
        nodes.push_back(std::move(owner));
        return result;
    }
    void Publish(ir::Block& body, ir::Inst* inst, ir::Inst* before) {
        body.InsertBefore(inst, before);
        inserted.push_back(inst);
    }
    void Unlink(ir::Block& body) {
        for (auto* inst : inserted) body.RemoveInst(inst);
        inserted.clear();
    }
    void ReleasePublished() {
        for (auto& owner : nodes)
            if (std::find(inserted.begin(), inserted.end(), owner.get()) != inserted.end())
                owner.release();
        inserted.clear();
    }
};
} // namespace

CfgStatePromoteStats PromoteCfgState(backend::FunctionPlan& function,
                                    u32 extent, std::span<const u32> lane_offsets,
                                    const CfgRegisterPlan& graph, CfgStatePromoteOptions options) {
    CfgStatePromoteStats stats;
    const auto reject = [&](CfgStatePromoteRejection reason) {
        stats.accepted = false; stats.rejection = reason;
        stats.entry_seeds = stats.phis_inserted = stats.loads_replaced = 0;
        stats.state_phis = stats.loads_forwarded = 0;
        return stats;
    };
    auto* body = function.body.get();
    if (!body || !graph.Enabled() || !graph.MatchesBody(body))
        return reject(CfgStatePromoteRejection::InvalidGraph);
    const auto& insts = graph.Instructions();
    const auto& blocks = graph.Blocks();
    const auto& edges = graph.Edges();
    std::vector<u32> lanes(lane_offsets.begin(), lane_offsets.end());
    std::sort(lanes.begin(), lanes.end());
    for (size_t i = 0; i < lanes.size(); ++i)
        if ((lanes[i] & 7) || !Range(lanes[i], 8, extent) ||
            (i && lanes[i] == lanes[i - 1]))
            return reject(CfgStatePromoteRejection::InvalidLanes);
    constexpr size_t max_cells = 1u << 20;
    if (lanes.size() > 64 || (!lanes.empty() &&
            (insts.size() > max_cells / lanes.size() || blocks.size() > max_cells / lanes.size())))
        return reject(CfgStatePromoteRejection::Budget);
    if (lanes.empty() || blocks.empty()) {
        stats.accepted = true; stats.rejection = CfgStatePromoteRejection::None;
        return stats;
    }
    if (insts.empty() || insts.front()->GetOp() != O::BindLabel)
        return reject(CfgStatePromoteRejection::InvalidRoot);

    // Cull lanes with no actual scalar load. A seed/Phi must ultimately be
    // reachable from a replaced load, not merely a declared state field.
    std::erase_if(lanes, [&](u32 lane) {
        for (u32 b = 0; b < blocks.size(); ++b) if (blocks[b].reachable)
            for (u32 p = blocks[b].begin; p < blocks[b].end; ++p)
                if (Classify(insts[p], lane, extent).load) return false;
        return true;
    });
    stats.lanes = lanes.size();
    stats.candidate_lanes = lanes.size();
    const size_t width = lanes.size();
    if (!width) { stats.accepted = true; stats.rejection = CfgStatePromoteRejection::None; return stats; }
    std::vector<std::vector<Access>> access(insts.size(), std::vector<Access>(width));
    for (u32 p = 0; p < insts.size(); ++p) {
        if (insts[p]->GetOp() == O::StoreUniform) ++stats.stores_retained;
        bool barrier{};
        for (u32 lane = 0; lane < width; ++lane) {
            access[p][lane] = Classify(insts[p], lanes[lane], extent);
            if (access[p][lane].load) ++stats.loads_seen;
            barrier |= access[p][lane].barrier;
            stats.partial_kills += access[p][lane].partial_store;
        }
        stats.barriers += barrier;
    }

    // Backward demand: a complete definition or barrier cuts the dependency
    // on incoming state. Reads require a value before that point. No weighted
    // frequency assumptions or store elimination participate in this proof.
    std::vector<Bits> needed(blocks.size(), Bits(width));
    bool changed;
    do {
        changed = false; ++stats.iterations;
        for (u32 b = blocks.size(); b-- > 0;) if (blocks[b].reachable) {
            Bits next(width);
            for (auto e : blocks[b].outgoing_edges)
                for (u32 lane = 0; lane < width; ++lane) next[lane] |= needed[edges[e].to][lane];
            for (u32 p = blocks[b].end; p-- > blocks[b].begin;)
                for (u32 lane = 0; lane < width; ++lane) {
                    const auto& a = access[p][lane];
                    if (a.barrier || a.partial_store || a.full_store) next[lane] = 0;
                    if (a.load) next[lane] = 1;
                }
            if (next != needed[b]) { needed[b] = std::move(next); changed = true; }
        }
        if (changed && stats.iterations > blocks.size() * width + 1)
            return reject(CfgStatePromoteRejection::Budget);
    } while (changed);

    // Must availability descends from top. The sole host-entry boundary is a
    // real preheader load for each demanded lane; kills are false, full
    // definitions are true. Every reachable cyclic proof therefore has an
    // initialized external path, rather than a self-justifying SSA identity.
    std::vector<Bits> in(blocks.size(), Bits(width, 1)), out(blocks.size(), Bits(width, 1));
    u32 availability_iterations{};
    do {
        changed = false; ++availability_iterations; ++stats.iterations;
        for (u32 b = 0; b < blocks.size(); ++b) if (blocks[b].reachable) {
            Bits next(width, 1);
            bool predecessor{};
            for (auto e : blocks[b].incoming_edges) {
                const auto& edge = edges[e];
                if (!edge.host_entry && !blocks[edge.from].reachable) continue;
                predecessor = true;
                for (u32 lane = 0; lane < width; ++lane)
                    next[lane] &= edge.host_entry ? needed[0][lane] : out[edge.from][lane];
            }
            if (!predecessor) return reject(CfgStatePromoteRejection::InvalidGraph);
            if (next != in[b]) { in[b] = next; changed = true; }
            for (u32 p = blocks[b].begin; p < blocks[b].end; ++p)
                for (u32 lane = 0; lane < width; ++lane) {
                    const auto& a = access[p][lane];
                    if (a.barrier || a.partial_store) next[lane] = 0;
                    else if (a.full_store || a.full_load) next[lane] = 1;
                }
            if (next != out[b]) { out[b] = std::move(next); changed = true; }
        }
        if (changed && availability_iterations > blocks.size() * width + 1)
            return reject(CfgStatePromoteRejection::Budget);
    } while (changed);

    PendingInstructions pending;
    std::vector<ir::Inst*> seeds(width);
    for (u32 lane = 0; lane < width; ++lane)
        if (needed[0][lane]) seeds[lane] = pending.New(O::LoadUniform);
    std::vector<Phi> phis;
    std::vector<std::vector<ir::Value>> entry(blocks.size(), std::vector<ir::Value>(width));
    std::unordered_map<ir::Inst*, size_t> phi_index;
    for (u32 b = 0; b < blocks.size(); ++b) if (blocks[b].reachable)
        for (u32 lane = 0; lane < width; ++lane) if (in[b][lane] && needed[b][lane]) {
            auto* phi = pending.New(O::AddPhi);
            phi_index.emplace(phi, phis.size());
            phis.push_back({phi, b, lane, {}});
            entry[b][lane] = ir::Value{phi};
        }
    if (pending.nodes.size() + insts.size() > std::numeric_limits<u16>::max())
        return reject(CfgStatePromoteRejection::Budget);
    std::vector<std::vector<ir::Value>> outgoing(blocks.size(), std::vector<ir::Value>(width));
    std::vector<Replacement> replacements;
    for (u32 b = 0; b < blocks.size(); ++b) if (blocks[b].reachable) {
        auto state = entry[b];
        for (u32 p = blocks[b].begin; p < blocks[b].end; ++p)
            for (u32 lane = 0; lane < width; ++lane) {
                const auto& a = access[p][lane];
                if (a.barrier || a.partial_store) state[lane] = {};
                else if (a.full_store) state[lane] = ir::Value{a.stored}.SetCastType(T::U64);
                else if (a.load) {
                    if (state[lane].Defined())
                        replacements.push_back({insts[p], state[lane],
                            insts[p]->GetArg<ir::Uniform>(0), insts[p]->ReturnType(), a.byte, a.bytes, lane});
                    else if (a.full_load) state[lane] = ir::Value{insts[p]}.SetCastType(T::U64);
                }
            }
        outgoing[b] = std::move(state);
    }
    for (auto& phi : phis) for (auto e : blocks[phi.block].incoming_edges) {
        const auto& edge = edges[e];
        ir::Value input;
        if (edge.host_entry) input = ir::Value{seeds[phi.lane]};
        else if (blocks[edge.from].reachable) input = outgoing[edge.from][phi.lane];
        if ((edge.host_entry || blocks[edge.from].reachable) && !input.Defined())
            return reject(CfgStatePromoteRejection::InvalidResult);
        phi.inputs.push_back(input);
    }

    // Eliminate trivial Phi aliases, including identity loops. Parameters
    // on unreachable structural edges do not constrain an executed value.
    std::unordered_map<ir::Inst*, ir::Value> aliases;
    const auto resolve = [&](ir::Value value) {
        for (size_t n = 0; value.Defined() && n <= phis.size(); ++n) {
            const auto found = aliases.find(value.Def());
            if (found == aliases.end()) break;
            value = found->second;
        }
        return value;
    };
    do {
        changed = false;
        for (const auto& phi : phis) if (!aliases.contains(phi.inst)) {
            ir::Value candidate;
            bool same = true;
            for (auto input : phi.inputs) if (input.Defined()) {
                input = resolve(input);
                if (input.Def() == phi.inst) continue;
                if (!candidate.Defined()) candidate = input;
                else if (candidate.Def() != input.Def()) { same = false; break; }
            }
            if (same && candidate.Defined()) { aliases.emplace(phi.inst, candidate); changed = true; }
        }
    } while (changed);
    for (auto& replacement : replacements) replacement.source = resolve(replacement.source);
    std::unordered_map<ir::Inst*, bool> retained;
    const auto retain_dependencies = [&] {
        retained.clear();
        std::vector<ir::Inst*> work;
        for (const auto& replacement : replacements) work.push_back(replacement.source.Def());
        for (size_t next = 0; next < work.size(); ++next) {
            auto* value = work[next];
            if (!retained.emplace(value, true).second) continue;
            if (const auto found = phi_index.find(value); found != phi_index.end())
                for (auto input : phis[found->second].inputs)
                    if (input.Defined()) work.push_back(resolve(input).Def());
        }
    };
    retain_dependencies();
    if (options.reject_dirty_loop_phis) {
        // Iterative Kosaraju over executable edges. SCCs include self loops,
        // duplicate edge occurrences and irreducible loops; host entry is
        // not an executable edge from the root back to itself.
        std::vector<bool> visited(blocks.size());
        std::vector<u32> order;
        for (u32 root = 0; root < blocks.size(); ++root) if (blocks[root].reachable && !visited[root]) {
            std::vector<std::pair<u32, size_t>> walk{{root, 0}};
            visited[root] = true;
            while (!walk.empty()) {
                auto& [b, next] = walk.back();
                if (next == blocks[b].outgoing_edges.size()) { order.push_back(b); walk.pop_back(); }
                else {
                    const auto& edge = edges[blocks[b].outgoing_edges[next++]];
                    if (!edge.host_entry && blocks[edge.to].reachable && !visited[edge.to]) {
                        visited[edge.to] = true; walk.emplace_back(edge.to, 0);
                    }
                }
            }
        }
        std::vector<u32> component(blocks.size(), UINT32_MAX), sizes;
        for (auto root = order.rbegin(); root != order.rend(); ++root) if (component[*root] == UINT32_MAX) {
            const u32 id = sizes.size(); sizes.push_back(0);
            std::vector<u32> work{*root}; component[*root] = id;
            for (size_t next = 0; next < work.size(); ++next) {
                const auto b = work[next]; ++sizes[id];
                for (auto e : blocks[b].incoming_edges) {
                    const auto& edge = edges[e];
                    if (!edge.host_entry && blocks[edge.from].reachable && component[edge.from] == UINT32_MAX) {
                        component[edge.from] = id; work.push_back(edge.from);
                    }
                }
            }
        }
        std::vector<bool> cyclic(sizes.size());
        for (u32 c = 0; c < sizes.size(); ++c) cyclic[c] = sizes[c] > 1;
        for (const auto& edge : edges)
            if (!edge.host_entry && blocks[edge.from].reachable && edge.from == edge.to)
                cyclic[component[edge.from]] = true;
        std::vector<Bits> dirty(sizes.size(), Bits(width));
        for (u32 b = 0; b < blocks.size(); ++b) if (blocks[b].reachable && cyclic[component[b]])
            for (u32 p = blocks[b].begin; p < blocks[b].end; ++p)
                for (u32 lane = 0; lane < width; ++lane) {
                    const auto& a = access[p][lane];
                    dirty[component[b]][lane] |= a.full_store || a.partial_store || a.barrier;
                }
        Bits excluded(width);
        for (const auto& phi : phis) if (retained.contains(phi.inst) && !aliases.contains(phi.inst)) {
            const auto c = component[phi.block];
            if (cyclic[c] && dirty[c][phi.lane]) excluded[phi.lane] = 1;
        }
        stats.rejected_dirty_loop_lanes = std::count(excluded.begin(), excluded.end(), u8{1});
        std::erase_if(replacements, [&](const auto& replacement) { return excluded[replacement.lane]; });
        retain_dependencies();
        // A surviving replacement must never depend on a removed lane's
        // provisional definition. Reject transactionally if that assumption
        // changes as the pass gains new cross-lane transformations.
        for (const auto& phi : phis)
            if (excluded[phi.lane] && retained.contains(phi.inst))
                return reject(CfgStatePromoteRejection::InvalidResult);
    }
    if (replacements.empty()) {
        stats.accepted = true; stats.rejection = CfgStatePromoteRejection::None;
        return stats;
    }
    for (u32 lane = 0; lane < width; ++lane) if (seeds[lane] && retained.contains(seeds[lane])) {
        seeds[lane]->SetArgs(ir::Uniform{lanes[lane], T::U64});
        ++stats.entry_seeds;
    }
    for (const auto& phi : phis) if (retained.contains(phi.inst)) {
        ir::Params params;
        for (auto input : phi.inputs) {
            if (input.Defined()) params.Push(resolve(input).SetCastType(T::U64));
            else params.Push(ir::Imm{u64{0}});
        }
        phi.inst->SetArgs(params);
        ++stats.phis_inserted;
    }

    // Mutate only after the complete proof and bounded plan exist. Preserve
    // every old pointer, metadata key, StoreUniform operand and relative order.
    pending.inserted.reserve(pending.nodes.size());
    size_t rewritten{};
    const auto rollback = [&] {
        for (size_t i = 0; i < rewritten; ++i) {
            const auto& replacement = replacements[i];
            replacement.load->Reset();
            replacement.load->LoadUniform(replacement.uniform).SetReturn(replacement.type);
        }
        pending.Unlink(*body);
    };
    try {
        for (auto* seed : seeds)
            if (seed && retained.contains(seed)) pending.Publish(*body, seed, insts.front());
        for (u32 b = 0; b < blocks.size(); ++b) {
            u32 first = blocks[b].begin;
            if (insts[first]->GetOp() == O::BindLabel) ++first;
            while (first < blocks[b].end && insts[first]->GetOp() == O::AddPhi) ++first;
            // Inserting before the next block is also after an all-Phi header.
            if (first == insts.size()) {
                for (const auto& phi : phis) if (phi.block == b && retained.contains(phi.inst)) {
                    body->InsertAfter(phi.inst, &body->GetInstList().back());
                    pending.inserted.push_back(phi.inst);
                }
            } else {
                for (const auto& phi : phis) if (phi.block == b && retained.contains(phi.inst))
                    pending.Publish(*body, phi.inst, insts[first]);
            }
        }
        for (const auto& replacement : replacements) {
            replacement.load->Reset();
            replacement.load->BitExtract(replacement.source, ir::Imm{replacement.byte * 8},
                                         ir::Imm{replacement.bytes * 8}).SetReturn(replacement.type);
            ++rewritten;
        }
        const auto verified = CfgRegisterPlan::ValidateGraph(body, function);
        if (!verified.Enabled()) {
            rollback();
            return reject(CfgStatePromoteRejection::InvalidResult);
        }
    } catch (...) {
        rollback();
        return reject(CfgStatePromoteRejection::Budget);
    }
    stats.loads_replaced = replacements.size();
    stats.loads_forwarded = stats.loads_replaced;
    stats.state_phis = stats.phis_inserted;
    body->ReIdInstr();
    pending.ReleasePublished();
    stats.accepted = true; stats.rejection = CfgStatePromoteRejection::None;
    return stats;
}
} // namespace swift::runtime::backend::x64

