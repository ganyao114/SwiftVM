#include "loop_hoist.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "runtime/compile/analysis/facts.h"
#include "runtime/compile/analysis/effects.h"
#include "runtime/compile/analysis/guest_state.h"
#include "runtime/compile/analysis/uses.h"
#include "runtime/compile/unit.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {
namespace {
using namespace ir;

bool HasSelfEdge(const Terminal& terminal, Location self) {
    return VisitVariant<bool>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, terminal::LinkBlock> ||
                      std::is_same_v<T, terminal::LinkBlockFast>) return term.next == self;
        else if constexpr (std::is_same_v<T, terminal::If> ||
                           std::is_same_v<T, terminal::Condition>)
            return HasSelfEdge(term.then_, self) || HasSelfEdge(term.else_, self);
        else if constexpr (std::is_same_v<T, terminal::CheckHalt>)
            return HasSelfEdge(term.else_, self);
        else if constexpr (std::is_same_v<T, terminal::Switch>)
            return std::any_of(term.cases.begin(), term.cases.end(),
                    [&](const auto& edge) { return HasSelfEdge(edge.then, self); });
        else return false;
    });
}

bool HasConditionalSelfEdge(const Block& block) {
    return VisitVariant<bool>(block.GetTerminal(), [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, terminal::If> ||
                      std::is_same_v<T, terminal::Condition>)
            return HasSelfEdge(term.then_, block.GetStartLocation()) ||
                   HasSelfEdge(term.else_, block.GetStartLocation());
        else
            return false;
    });
}

const Inst* ScalarRoot(Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        const auto input = value.Def()->GetArg<Value>(0);
        if (IsFloatValueType(value.Def()->ReturnType()) != IsFloatValueType(input.Type())) break;
        value = input;
    }
    return value.Def();
}

bool Scalar(const Inst& inst) {
    return inst.HasValue() && inst.ReturnType() != ValueType::VOID &&
           !IsFloatValueType(inst.ReturnType());
}

bool WideConstant(const Inst& inst) {
    const auto bits = inst.GetArg<Imm>(0).Get();
    u32 pieces{};
    for (u32 shift = 0; shift < 64; shift += 16) pieces += ((bits >> shift) & 0xffff) != 0;
    return pieces >= 2;
}

struct ValueSpan {
    const Block* owner{};
    u32 begin{};
    u32 end{};
    u32 uses{};
    bool cross_block{};
    const Inst* arithmetic{};
    u32 arithmetic_uses{};
    FlagsDemand flags{};
};

struct FunctionSummary {
    using LiveSet = std::unordered_set<const Inst*>;
    std::unordered_map<const Inst*, ValueSpan> values;
    std::unordered_map<const Block*, LiveSet> incoming, outgoing;
    std::vector<GuestStateRange> writes;
    bool unknown_writes{};

    void StateWrite(const Inst& inst, const GuestStateLayout& layout) {
        if (inst.GetOp() == OpCode::StoreUniform) {
            const auto uniform = inst.GetArg<Uniform>(0);
            writes.push_back({uniform.GetOffset(), uniform.GetOffset() +
                    GetValueSizeByte(inst.GetArg<Value>(1).Type())});
        } else if (True(inst.GetEffects() & IREffect::Call)) {
            const auto* effects =
                    HelperUniformWrites(inst, layout.uniform_size, UniformWritePolicy::NoReentry);
            if (!effects) {
                unknown_writes = true;
                return;
            }
            for (u32 i = 0; i < effects->count; ++i) {
                const auto& range = effects->ranges[i];
                writes.push_back({range.offset, range.offset + range.size});
            }
        } else if (inst.GetOp() == OpCode::UniformBarrier || inst.GetOp() == OpCode::XchgBarrier ||
                   True(inst.GetEffects() & IREffect::Unknown)) unknown_writes = true;
    }

    FunctionSummary(const HIRFunction& function, const GuestStateLayout& layout) {
        for (auto* current : function.GetHIRBlocks()) {
            if (!current) continue;
            auto* block = current->GetBlock();
            u32 point{};
            for (auto& inst : block->GetInstList()) {
                if (inst.HasValue()) values.emplace(&inst, ValueSpan{block, point, point});
                StateWrite(inst, layout);
                ++point;
            }
        }
        for (auto* current : function.GetHIRBlocks()) {
            if (!current) continue;
            auto* block = current->GetBlock();
            const auto add = [&](Value value, u32 point, const Inst* user) {
                const auto found = values.find(value.Def());
                if (found != values.end()) {
                    auto& span = found->second;
                    ++span.uses;
                    span.cross_block |= span.owner != block;
                    if (span.owner == block) span.end = std::max(span.end, point);
                    if (user && (user->GetOp() == OpCode::Add || user->GetOp() == OpCode::Sub)) {
                        span.arithmetic = user;
                        ++span.arithmetic_uses;
                    }
                    if (user && (user->GetOp() == OpCode::SaveFlags ||
                            user->GetOp() == OpCode::BranchOnlyFlags || user->GetOp() == OpCode::GetFlags)) {
                        span.flags.requested |= user->GetArg<Flags>(1);
                        ++span.flags.pseudo_count;
                        span.flags.has_save |= user->GetOp() == OpCode::SaveFlags;
                        span.flags.branch_only |= user->GetOp() == OpCode::BranchOnlyFlags;
                    }
                }
                auto* root = ScalarRoot(value);
                if (root != value.Def()) {
                    auto it = values.find(root);
                    if (it != values.end()) {
                        it->second.cross_block |= it->second.owner != block;
                        if (it->second.owner == block)
                            it->second.end = std::max(it->second.end, point);
                    }
                }
            };
            u32 point{};
            for (auto& inst : block->GetInstList()) {
                inst.ForEachValue([&](Value value) { add(value, point, &inst); });
                ++point;
            }
            VisitTerminalValues(block->GetTerminal(),
                                [&](Value value) { add(value, point ? point - 1 : 0, nullptr); });
        }
        const bool cross = std::any_of(values.begin(), values.end(), [](const auto& item) {
            return item.second.cross_block && Scalar(*item.first) &&
                   ScalarRoot(Value{item.first}) == item.first;
        });
        if (cross) BuildLiveness(function);
    }

    void BuildLiveness(const HIRFunction& function) {
        std::vector<const HIRBlock*> blocks;
        std::unordered_map<const Block*, LiveSet> definitions, uses;
        for (auto* current : function.GetHIRBlocks()) {
            if (!current) continue;
            blocks.push_back(current);
            auto* block = current->GetBlock();
            incoming.emplace(block, LiveSet{});
            outgoing.emplace(block, LiveSet{});
            const auto add = [&](Value value) {
                auto* root = ScalarRoot(value);
                const auto found = values.find(root);
                if (found != values.end() && found->second.cross_block && Scalar(*root) &&
                    found->second.owner != block) uses[block].insert(root);
            };
            for (auto& inst : block->GetInstList()) {
                const auto found = values.find(&inst);
                if (found != values.end() && found->second.cross_block && Scalar(inst) &&
                    ScalarRoot(Value{&inst}) == &inst) definitions[block].insert(&inst);
                inst.ForEachValue(add);
            }
            VisitTerminalValues(block->GetTerminal(), add);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
                auto* block = (*it)->GetBlock();
                LiveSet out;
                for (const auto& edge : (*it)->GetOutgoingEdges()) {
                    if (!edge.GetTargetBlock()) continue;
                    const auto next = incoming.find(edge.GetTargetBlock()->GetBlock());
                    if (next != incoming.end()) out.insert(next->second.begin(), next->second.end());
                }
                LiveSet in = uses[block];
                for (auto* value : out) if (!definitions[block].contains(value)) in.insert(value);
                if (incoming[block] != in || outgoing[block] != out) {
                    incoming[block] = std::move(in);
                    outgoing[block] = std::move(out);
                    changed = true;
                }
            }
        }
    }

    bool LiveIn(Block* block, const Inst* value) const {
        const auto found = incoming.find(block);
        return found != incoming.end() && found->second.contains(value);
    }
    bool LiveOut(Block* block, const Inst* value) const {
        const auto found = outgoing.find(block);
        return found != outgoing.end() && found->second.contains(value);
    }

    bool ReadOnly(u32 begin, u32 size) const {
        return !unknown_writes && std::none_of(writes.begin(), writes.end(), [&](const auto& range) {
            return begin < range.end && range.begin < begin + size;
        });
    }
};

// Only candidate-bearing units build this temporary upper bound. CFG facts
// are needed only for real cross-block GPR values. Local spans use stable
// pointers and ordinals, never final allocation IDs.
class LoopPressure {
public:
    LoopPressure(Block& block, const FunctionSummary& summary,
                 const LoopHoistTarget& target, const FeatureSet& features)
            : registers(target.value_registers), stable(registers) {
        const auto count = block.GetInstList().size();
        sites.reserve(count);
        std::vector<s32> changes(count + 1);
        std::unordered_map<const Inst*, u32> foreign_ends;
        const auto add = [&](Value value, u32 point) {
            auto* root = ScalarRoot(value);
            const auto found = summary.values.find(root);
            if (found != summary.values.end() && found->second.owner != &block && Scalar(*root))
                foreign_ends[root] = point;
        };
        for (auto& inst : block.GetInstList()) {
            inst.ForEachValue([&](Value value) { add(value, sites.size()); });
            const auto found = summary.values.find(&inst);
            const auto demand = found != summary.values.end() ? found->second.flags : FlagsDemand{};
            sites.push_back(target.describe(registers, inst, demand, features));
            stable &= ~sites.back().clobbers;
        }
        VisitTerminalValues(block.GetTerminal(),
                            [&](Value value) { add(value, count ? count - 1 : 0); });
        for (const auto& [inst, span] : summary.values) {
            if (!Scalar(*inst) || ScalarRoot(Value{inst}) != inst) continue;
            u32 begin = span.begin, end = span.end;
            if (span.owner != &block) {
                if (!summary.LiveIn(&block, inst)) continue;
                begin = 0;
                const auto local = foreign_ends.find(inst);
                end = summary.LiveOut(&block, inst) || local == foreign_ends.end() ? count - 1 : local->second;
            } else if (summary.LiveOut(&block, inst)) end = count - 1;
            const auto fixed = span.owner == &block ? sites[begin].fixed_definition : 0u;
            if (!span.cross_block && fixed && !(fixed & registers)) {
                bool overwritten{};
                for (u32 point = begin + 1; point <= end; ++point)
                    overwritten |= (sites[point].clobbers & fixed) != 0;
                if (!overwritten) continue;
            }
            ++changes[begin];
            --changes[end + 1];
        }
        s32 live{};
        for (u32 point = 0; point < count; ++point) {
            live += changes[point];
            ASSERT(live >= 0);
            baseline.push_back(live);
        }
        current = baseline;
    }

    bool Add(const ValueSpan& span) {
        if (anchors + 1 > std::popcount(stable)) return false;
        for (u32 point = 0; point < current.size(); ++point) {
            const u32 extra = point < span.begin || point > span.end;
            const u32 available = std::popcount(registers & ~sites[point].clobbers);
            const u32 capacity = available > sites[point].scratch ? available - sites[point].scratch : 0;
            // Do not introduce pressure excess at any point, including a
            // pre-existing saturated window. RA still verifies real mapping.
            if (current[point] + extra > std::max(capacity, baseline[point])) return false;
        }
        for (u32 point = 0; point < current.size(); ++point)
            current[point] += point < span.begin || point > span.end;
        ++anchors;
        return true;
    }

private:
    u32 registers;
    u32 stable;
    u32 anchors{};
    std::vector<HoistRegisterSite> sites;
    std::vector<u32> baseline, current;
};

}  // namespace

LoopHoistResult HoistLoopInvariants(ir::HIRFunction* function, const GuestStateLayout& layout,
                                  CompileUnit& unit, const LoopHoistTarget& target,
                                  const FeatureSet& features) {
    LoopHoistResult result;
    if (!function || !target.describe || !target.value_registers ||
        (!features.loop_gpr_hoist && !features.loop_const_hoist)) return result;
    std::vector<Block*> loops;
    for (auto& current : function->GetHIRBlocksRPO()) {
        auto* block = current.GetBlock();
        if (!block->HasTerminal() || !block->GetLoopHoistMetadata().anchors.empty() ||
            !HasSelfEdge(block->GetTerminal(), block->GetStartLocation())) continue;
        const bool conditional = HasConditionalSelfEdge(*block);
        const bool candidate = std::any_of(block->GetInstList().begin(), block->GetInstList().end(),
                [&](const Inst& inst) {
                    if (inst.ReturnType() != ValueType::U64) return false;
                    if (features.loop_const_hoist && conditional && inst.GetOp() == OpCode::LoadImm)
                        return WideConstant(inst);
                    return features.loop_gpr_hoist && inst.GetOp() == OpCode::LoadUniform &&
                        layout.IsLoopInvariantGPRRange(inst.GetArg<Uniform>(0).GetOffset(), sizeof(u64));
                });
        if (candidate) loops.push_back(function->EditBlock(&current).GetBlock());
    }
    if (loops.empty()) return result;
    FunctionSummary summary{*function, layout};
    for (auto* block : loops) {
        StackVector<const Inst*, 4> anchors;
        u16 gprs{}, constants{};
        const bool conditional = HasConditionalSelfEdge(*block);
        std::optional<LoopPressure> pressure;
        for (auto& inst : block->GetInstList()) {
            if (inst.ReturnType() != ValueType::U64) continue;
            const auto found = summary.values.find(&inst);
            if (found == summary.values.end() || !found->second.uses || found->second.cross_block) continue;
            const auto& span = found->second;
            bool gpr{};
            if (features.loop_gpr_hoist && gprs == 0 && inst.GetOp() == OpCode::LoadUniform) {
                const auto offset = inst.GetArg<Uniform>(0).GetOffset();
                gpr = layout.IsLoopInvariantGPRRange(offset, sizeof(u64)) && summary.ReadOnly(offset, sizeof(u64));
                for (u32 byte = 0; gpr && byte < sizeof(u64); ++byte)
                    gpr &= layout.uniform_regs_map.GetValueAt(offset + byte).Null();
            }
            const bool constant = features.loop_const_hoist && conditional &&
                    inst.GetOp() == OpCode::LoadImm && WideConstant(inst) &&
                    span.arithmetic_uses == 1 && span.arithmetic &&
                    (summary.values.at(span.arithmetic).flags.has_save ||
                     summary.values.at(span.arithmetic).flags.branch_only);
            if (!gpr && !constant) continue;
            ++result.candidates;
            if (!pressure) pressure.emplace(*block, summary, target, features);
            if (!pressure->Add(span)) { ++result.pressure_rejections; continue; }
            anchors.push_back(&inst);
            gprs += gpr;
            constants += constant;
        }
        if (anchors.empty()) continue;
        // Allocate owning metadata before changing the instruction list.
        unit.SetLoopHoistMetadata(block, {anchors.back(), anchors, gprs, constants});
        for (auto it = anchors.rbegin(); it != anchors.rend(); ++it) {
            block->MoveBefore(*it, &block->GetInstList().front());
        }
        result.gprs += gprs;
        result.constants += constants;
    }
    return result;
}

}  // namespace swift::runtime::compile
