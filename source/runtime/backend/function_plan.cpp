#include "function_plan.h"

#include <stdexcept>
#include <limits>
#include <algorithm>
#include <optional>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend {
namespace {
using O = ir::OpCode;

class Builder {
public:
    Builder(ir::HIRFunction* function, std::span<ir::HIRBlock* const> blocks)
        : function(function), blocks(blocks) {
        plan.body = new ir::Block(function->GetFunction()->GetStartLocation());
        for (auto* block : blocks) {
            auto* label = New(O::BindLabel);
            label->SetArgs(ir::Value{label});
            labels.emplace(block, label);
            plan.locations.emplace(label, block->GetBlock()->GetStartLocation());
            locations.emplace(block->GetBlock()->GetStartLocation().Value(), block);
            for (auto& inst : block->GetInstList()) {
                auto* clone = New(inst.GetOp());
                clone->SetReturn(inst.ReturnType());
                clones.emplace(&inst, clone);
            }
        }
    }

    ~Builder() { for (auto& [inst, owner] : owned) inst->ReleaseArgs(); }
    FunctionPlan Run() {
        // Construct arguments before emitting terminals, including forward
        // phi references and pseudo producer/use chains.
        for (auto* block : blocks) for (auto& inst : block->GetInstList()) CloneArgs(&inst);
        for (auto* block : blocks) {
            Append(labels.at(block));
            bool header = true;
            for (auto& inst : block->GetInstList()) {
                if (header && inst.GetOp() != O::AddPhi) {
                    SetLocation(block); header = false;
                }
                Append(clones.at(&inst));
            }
            if (header) SetLocation(block);
            const auto terminal = block->GetBlock()->GetTerminal();
            BindTerminalInputs(block, terminal);
            Terminal(block, terminal);
        }
        // HIR phi arguments follow HIR predecessor order. The local lowering
        // follows emitted edge order; expand duplicate terminal edges and
        // reorder once, while preserving parallel-copy semantics at runtime.
        for (auto* block : blocks) {
            for (auto& inst : block->GetInstList()) {
                if (inst.GetOp() != O::AddPhi) break;
                std::vector<ir::DataClass> inputs;
                for (const auto& param : inst.GetArg<ir::Params>(0)) inputs.push_back(param.data);
                const auto predecessors = block->GetPredecessors();
                if (inputs.size() != predecessors.size())
                    throw std::runtime_error("shared function phi input count differs from HIR predecessors");
                const auto input = [&](ir::HIRBlock* predecessor) {
                    std::optional<ir::DataClass> result;
                    for (size_t i = 0; i < predecessors.size(); ++i)
                        if (predecessors[i] == predecessor) {
                            if (result) throw std::runtime_error("shared function has ambiguous duplicate host-entry edge");
                            result = inputs[i];
                        }
                    if (result) return *result;
                    throw std::runtime_error("shared function terminal edge is absent from HIR predecessors");
                };
                ir::Params params;
                const auto push = [&](const ir::DataClass& data) {
                    if (data.IsValue()) params.Push(Remap(data.value));
                    else if (data.IsImm()) params.Push(data.imm);
                    else throw std::runtime_error("invalid shared function phi input");
                };
                if (block == blocks.front()) push(input(function->GetEntryBlock()));
                for (const auto& edge : edges) if (edge.to == block) push(inputs.at(edge.input_slot));
                clones.at(&inst)->SetArg(0, params);
            }
        }
        plan.body->SetTerminal(ir::terminal::ReturnToDispatch{});
        if (plan.body->GetInstList().size() > std::numeric_limits<u16>::max())
            throw std::runtime_error("shared function CFG exceeds the IR instruction ID limit");
        plan.body->ReIdInstr();
        return std::move(plan);
    }

private:
    struct Edge { ir::HIRBlock* from; ir::HIRBlock* to; size_t input_slot; };
    // HIRBuilder::If inserts else before then; Switch inserts case order.
    // Retain occurrence identity, rather than searching by source alone.
    // HIR Edge has no terminal path field, so reject duplicate leaves in a
    // manually nested tree where their insertion order cannot be proven.
    void BindTerminalInputs(ir::HIRBlock* source, const ir::Terminal& terminal) {
        terminal_inputs.clear();
        std::vector<std::pair<const ir::Terminal*, ir::HIRBlock*>> leaves;
        const auto collect = [&](const auto& recurse, const ir::Terminal& value) -> void {
            VisitVariant<void>(value, [&](const auto& term) {
                using T = std::decay_t<decltype(term)>;
                if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> || std::is_same_v<T, ir::terminal::LinkBlockFast>) {
                    if (auto found = locations.find(term.next.Value()); found != locations.end()) leaves.emplace_back(&value, found->second);
                } else if constexpr (std::is_same_v<T, ir::terminal::If> || std::is_same_v<T, ir::terminal::Condition>) {
                    recurse(recurse, term.else_); recurse(recurse, term.then_);
                } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                    for (const auto& item : term.cases) recurse(recurse, item.then);
                } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, term.else_);
            });
        };
        collect(collect, terminal);
        const auto leaf = [](const ir::Terminal& value) {
            return VisitVariant<bool>(value, [](const auto& term) {
                using T = std::decay_t<decltype(term)>;
                return !std::is_same_v<T, ir::terminal::If> && !std::is_same_v<T, ir::terminal::Condition> &&
                       !std::is_same_v<T, ir::terminal::Switch> && !std::is_same_v<T, ir::terminal::CheckHalt>;
            });
        };
        const bool known_order = VisitVariant<bool>(terminal, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) return leaf(term.then_) && leaf(term.else_);
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                return std::all_of(term.cases.begin(), term.cases.end(), [&](const auto& item) { return leaf(item.then); });
            } else return false;
        });
        std::unordered_map<ir::HIRBlock*, size_t> ordinal;
        for (const auto& [value, target] : leaves) {
            const size_t occurrence = ordinal[target]++;
            if (occurrence && !known_order) throw std::runtime_error("shared function cannot identify duplicate nested terminal edges");
            const auto predecessors = target->GetPredecessors();
            size_t seen{}; bool found{};
            for (size_t i = 0; i < predecessors.size(); ++i) if (predecessors[i] == source) {
                if (seen++ == occurrence) { terminal_inputs.emplace(value, i); found = true; break; }
            }
            if (!found) throw std::runtime_error("shared function terminal edge is absent from HIR predecessors");
        }
        for (const auto& [target, expected] : ordinal) {
            const auto predecessors = target->GetPredecessors();
            if (std::count(predecessors.begin(), predecessors.end(), source) != static_cast<std::ptrdiff_t>(expected))
                throw std::runtime_error("shared function HIR predecessor occurrences differ from terminal edges");
        }
    }
    ir::Value Remap(ir::Value value) const {
        auto found = clones.find(value.Def());
        if (found == clones.end()) throw std::runtime_error("shared SSA input belongs to another function");
        return ir::Value{found->second}.SetCastType(value.Type());
    }
    ir::Inst* New(O op) {
        auto owner = std::make_unique<ir::Inst>(op);
        auto* result = owner.get(); owned.emplace(result, std::move(owner)); return result;
    }
    void Append(ir::Inst* inst) {
        plan.body->AppendInst(inst);
        owned.at(inst).release(); owned.erase(inst);
    }
    template<class... Args> ir::Inst* Inst(O op, ir::ValueType type, const Args&... args) {
        auto* inst = New(op); inst->SetArgs(args...); inst->SetReturn(type); Append(inst); return inst;
    }
    void CloneArgs(ir::Inst* original) {
        auto* clone = clones.at(original);
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = original->ArgAt(slot);
            switch (arg.GetType()) {
                case ir::ArgType::Void: break;
                case ir::ArgType::Value: clone->SetArg(slot, Remap(arg.Get<ir::Value>())); break;
                case ir::ArgType::Imm: clone->SetArg(slot, arg.Get<ir::Imm>()); break;
                case ir::ArgType::Cond: clone->SetArg(slot, arg.Get<ir::Cond>()); break;
                case ir::ArgType::Flags: clone->SetArg(slot, arg.Get<ir::Flags>()); break;
                case ir::ArgType::Operand: clone->SetArg(slot, arg.Get<ir::Operand::Op>()); break;
                case ir::ArgType::Local: clone->SetArg(slot, arg.Get<ir::Local>()); break;
                case ir::ArgType::Uniform: clone->SetArg(slot, arg.Get<ir::Uniform>()); break;
                case ir::ArgType::Lambda: {
                    auto lambda = arg.Get<ir::Lambda>();
                    if (lambda.IsValue()) lambda.GetValue() = Remap(lambda.GetValue());
                    clone->SetArg(slot, lambda); break;
                }
                case ir::ArgType::Params: {
                    ir::Params params;
                    for (const auto& param : arg.Get<ir::Params>()) {
                        if (param.data.IsValue()) params.Push(Remap(param.data.value));
                        else params.Push(param.data.imm);
                    }
                    clone->SetArg(slot, params); break;
                }
            }
        }
    }
    void SetLocation(ir::HIRBlock* block) {
        if (block != blocks.front()) return; // Internal edges commit before the target poll.
        // This marker runs after the phi header. Guest state remains accurate
        // through callbacks, interrupts and faults in the shared frame.
        Inst(O::SetLocation, ir::ValueType::VOID,
             ir::Lambda{ir::Imm{block->GetBlock()->GetStartLocation().Value()}});
    }
    ir::Inst* Branch(ir::Value condition, ir::Inst* label, bool unconditional) {
        auto* branch = Inst(O::Goto, ir::ValueType::VOID, condition);
        plan.targets[branch] = label;
        if (unconditional) plan.stops.insert(branch);
        return branch;
    }
    ir::Value Always() {
        return ir::Value{Inst(O::LoadImm, ir::ValueType::U8, ir::Imm{u8{1}})};
    }
    void Terminal(ir::HIRBlock* source, const ir::Terminal& terminal) {
        VisitVariant<void>(terminal, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                          std::is_same_v<T, ir::terminal::LinkBlockFast>) {
                if (auto target = locations.find(term.next.Value()); target != locations.end()) {
                    Branch(Always(), labels.at(target->second), true);
                    edges.push_back({source, target->second, terminal_inputs.at(&terminal)});
                } else Exit(terminal);
            } else if constexpr (std::is_same_v<T, ir::terminal::If> || std::is_same_v<T, ir::terminal::Condition>) {
                auto* other = New(O::BindLabel); other->SetArgs(ir::Value{other});
                plan.synthetic_labels.insert(other);
                ir::Value condition;
                if constexpr (std::is_same_v<T, ir::terminal::If>) condition = Remap(term.cond);
                else condition = ir::Value{Inst(O::CondSet, ir::ValueType::U8, term.cond)};
                auto* branch = Inst(O::NotGoto, ir::ValueType::VOID, condition);
                plan.targets[branch] = other;
                Terminal(source, term.then_); Append(other); Terminal(source, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                if (term.value.Type() < ir::ValueType::U8 || term.value.Type() > ir::ValueType::V64)
                    throw std::runtime_error("shared function Switch requires a scalar selector");
                const auto bits = u32(ir::GetValueSizeByte(term.value.Type())) * 8;
                if (!bits || bits > 64) throw std::runtime_error("shared function Switch requires a scalar selector");
                for (const auto& item : term.cases) {
                    // Original terminals compare a normalized scalar against
                    // the full u64 immediate; an out-of-width case cannot
                    // match. Preserve its original occurrence in HIR mapping,
                    // but do not emit a truncated comparison/phantom edge.
                    if (bits < 64 && (item.case_value.Get() >> bits)) continue;
                    auto* next = New(O::BindLabel); next->SetArgs(ir::Value{next});
                    plan.synthetic_labels.insert(next);
                    auto* difference = Inst(O::Xor, term.value.Type(), Remap(term.value), ir::Operand{item.case_value});
                    auto* branch = Inst(O::Goto, ir::ValueType::VOID, ir::Value{difference});
                    plan.targets[branch] = next;
                    Terminal(source, item.then); Append(next);
                }
                Exit(ir::terminal::ReturnToDispatch{});
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                auto* marker = Inst(O::Nop, ir::ValueType::VOID);
                plan.halt_checks.insert(marker); Terminal(source, term.else_);
            } else Exit(terminal);
        });
    }
    void Exit(const ir::Terminal& terminal) {
        auto* marker = Inst(O::Nop, ir::ValueType::VOID);
        plan.exits.emplace(marker, terminal); plan.stops.insert(marker);
    }
    ir::HIRFunction* function;
    std::span<ir::HIRBlock* const> blocks;
    FunctionPlan plan;
    std::unordered_map<ir::Inst*, ir::Inst*> clones;
    std::unordered_map<ir::HIRBlock*, ir::Inst*> labels;
    std::unordered_map<u64, ir::HIRBlock*> locations;
    std::vector<Edge> edges;
    std::unordered_map<const ir::Terminal*, size_t> terminal_inputs;
    std::unordered_map<ir::Inst*, std::unique_ptr<ir::Inst>> owned;
};
} // namespace

FunctionPlan FunctionPlan::Build(ir::HIRFunction* function, std::span<ir::HIRBlock* const> blocks) {
    if (!function || blocks.empty() || !blocks.front() ||
        blocks.front()->GetBlock()->GetStartLocation() != function->GetFunction()->GetStartLocation())
        throw std::runtime_error("shared function CFG requires a decoded root");
    std::unordered_set<u64> locations;
    for (auto* block : blocks) {
        if (!block || block->GetBlock()->GetStartLocation() == ir::Location::INVALID ||
            !locations.insert(block->GetBlock()->GetStartLocation().Value()).second)
            throw std::runtime_error("shared function CFG has invalid or duplicate block locations");
    }
    return Builder{function, blocks}.Run();
}
} // namespace swift::runtime::backend
