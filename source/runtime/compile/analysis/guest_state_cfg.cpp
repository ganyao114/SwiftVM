#include "guest_state.h"

#include <algorithm>
#include <array>
#include <functional>

#include "base/common_funcs.h"
#include "semantic_facts.h"
#include "runtime/ir/block.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {
namespace {
using namespace ir;

// A logical byte is an SSA origin, a constant, or a replicated sign bit.
// A type view never certifies the unspecified high bits of a physical X reg.
struct LogicalByte {
    enum Kind : u8 { Unknown, ValueByte, Constant, Sign };
    const Inst* definition{};
    Kind kind{};
    u8 byte{}, constant{};
    bool operator==(const LogicalByte&) const = default;
};

LogicalByte ConstantByte(u8 byte) { return {nullptr, LogicalByte::Constant, 0, byte}; }

NOINLINE LogicalByte ByteOf(Value value, u32 byte) {
    bool sign{};
    while (value.Defined()) {
        const auto* def = value.Def();
        const u32 width = GetScalarIntegerBits(value.Type());
        const u32 defined_width = GetScalarIntegerBits(def->ReturnType());
        if (!width || byte >= width / 8 || byte >= defined_width / 8) return {};
        const auto op = def->GetOp();
        if (op == OpCode::LoadImm) {
            const auto constant = static_cast<u8>(def->GetArg<Imm>(0).Get() >> (byte * 8));
            return ConstantByte(sign ? (constant & 0x80 ? 0xff : 0) : constant);
        }
        if (op != OpCode::BitCast && op != OpCode::ZeroExtend32 &&
            op != OpCode::ZeroExtend32To64 && op != OpCode::ZeroExtend64 &&
            op != OpCode::SignExtend && op != OpCode::BitExtract)
            return {def, sign ? LogicalByte::Sign : LogicalByte::ValueByte,
                    static_cast<u8>(byte)};
        const auto input = def->GetArg<Value>(0);
        const u32 input_width = GetScalarIntegerBits(input.Type());
        // Sealed SSA normally decreases along these edges. Keep an opaque
        // identity on unsupported/nondecreasing aliases instead of chasing a
        // malformed cycle or guessing bits from a widened use-edge view.
        if (!input.Defined() || !input_width || input.Id() >= value.Id())
            return {def, sign ? LogicalByte::Sign : LogicalByte::ValueByte,
                    static_cast<u8>(byte)};
        if (op == OpCode::BitExtract) {
            const auto left = def->GetArg<Imm>(1).Get();
            const auto bits = def->GetArg<Imm>(2).Get();
            if (left % 8 || bits % 8 || !bits || left >= input_width ||
                bits > input_width - left)
                return {def, sign ? LogicalByte::Sign : LogicalByte::ValueByte,
                        static_cast<u8>(byte)};
            if (byte >= bits / 8) return ConstantByte(0);
            byte += static_cast<u32>(left / 8);
        } else if (op == OpCode::ZeroExtend32 || op == OpCode::ZeroExtend32To64 ||
                   op == OpCode::ZeroExtend64) {
            const auto source_bits = IntegerConversionInputBits(op, input_width);
            if (byte >= source_bits / 8) return ConstantByte(0);
        } else if (op == OpCode::SignExtend && byte >= input_width / 8) {
            byte = input_width / 8 - 1;
            sign = true;
        }
        value = input;
    }
    return {};
}

bool PreservesGPRBytes(const Inst& inst) {
    switch (inst.GetOp()) {
        case OpCode::CallLambda: {
            const auto helper = HelperStateEffects::Resolve(inst.GetArg<Lambda>(0));
            return helper.direct && !helper.MayReenter() && !helper.WritesGuestState();
        }
        case OpCode::SetHostFPR:
        case OpCode::AdvancePC:
        case OpCode::SetLocation:
            return true;
        case OpCode::GetUniformAddress:
        case OpCode::CallReturn:
            return false;
        default:
            break;
    }
    // Opaque uniform writes and aliased memory writes cannot establish which
    // logical slot survived. Read/fault observations themselves preserve it.
    constexpr auto writes = IREffect::WriteGuestState | IREffect::WriteMemory |
            IREffect::Call | IREffect::Barrier | IREffect::Unknown;
    return (inst.GetEffects() & writes) == IREffect::None;
}

class GuestStateVersions final {
public:
    GuestStateVersions(const Block* scope, GuestStateEntryPolicy policy)
            : scope(scope), policy(policy) {}

    const Block* Scope() const { return scope; }

    struct Answer {
        const Inst* before;
        const Inst* definition;
        ValueType type;
        u8 home, offset;
        bool matches{};
    };

    static bool Less(const Answer& a, const Answer& b) {
        if (a.before != b.before) return std::less<>{}(a.before, b.before);
        if (a.definition != b.definition) return std::less<>{}(a.definition, b.definition);
        if (a.type != b.type) return a.type < b.type;
        if (a.home != b.home) return a.home < b.home;
        return a.offset < b.offset;
    }

    std::optional<bool> Find(const Answer& key) const {
        const auto it = std::lower_bound(answers.begin(), answers.end(), key, Less);
        return it != answers.end() && !Less(key, *it) ? std::optional{it->matches} : std::nullopt;
    }

    bool Prepare(const Block* block, Answer key, Value expected, SemanticFacts& facts) {
        if (const auto found = Find(key)) return *found;
        key.matches = Prove(block, key, expected, facts);
        answers.insert(std::lower_bound(answers.begin(), answers.end(), key, Less), key);
        return key.matches;
    }

private:
    struct Input {
        const Block* block;
        std::span<const Inst* const> events;
        u32 first_label{}, label_count{}, exit_point{};
        bool closed{true};
    };
    struct Label { u32 event, source, point; };
    struct FunctionInputs {
        static constexpr u8 Reached = 1, Root = 2;
        const FunctionCFG* cfg{};
        // Only graph metadata is dense. Observation/label inputs are created
        // for the blocks actually visited by a proof, not the complete RPO.
        std::vector<u32> input_ids;
        std::vector<u8> states;
    };
    const Block* scope;
    GuestStateEntryPolicy policy;
    std::unique_ptr<FunctionInputs> function_inputs;
    std::vector<Input> inputs;
    std::vector<Label> labels;
    std::vector<Answer> answers;
    u32 point_count{};

    NOINLINE u32 PrepareInput(const Block* block, SemanticFacts& facts) {
        u32 graph_id = FunctionCFG::MissingBlock;
        if (function_inputs) {
            const auto id = function_inputs->cfg->FindBlock(block);
            ASSERT(id);
            graph_id = *id;
            const auto prepared = function_inputs->input_ids[graph_id];
            if (prepared != FunctionCFG::MissingBlock) return prepared;
        } else if (!inputs.empty()) {
            if (inputs.front().block == block) return 0;
            // Before an incoming state is needed, keep only one local input.
            // Published boolean answers for other blocks remain independent.
            inputs.clear(); labels.clear(); point_count = 0;
        }
        Input input{block, facts.PrepareGuestStateObservations(block)};
        const auto previous_labels = labels.size();
        const auto previous_points = point_count;
        input.exit_point = point_count++;
        try {
            PrepareLabels(input);
            inputs.push_back(input);
        } catch (...) {
            labels.resize(previous_labels);
            point_count = previous_points;
            throw;
        }
        const auto id = static_cast<u32>(inputs.size() - 1);
        if (function_inputs) function_inputs->input_ids[graph_id] = id;
        return id;
    }

    NOINLINE const FunctionInputs& PrepareFunction(SemanticFacts& facts) {
        if (function_inputs) return *function_inputs;
        auto next = std::make_unique<FunctionInputs>();
        next->cfg = &facts.PrepareFunctionCFG();
        const auto blocks = next->cfg->Blocks();
        next->input_ids.resize(blocks.size(), FunctionCFG::MissingBlock);
        next->states.resize(blocks.size(), 0);
        for (u32 i = 0; i < inputs.size(); ++i) {
            const auto id = next->cfg->FindBlock(inputs[i].block);
            ASSERT(id);
            next->input_ids[*id] = i;
        }
        StackVector<u32, 16> work;
        for (u32 i = 0; i < blocks.size(); ++i) {
            const auto& info = blocks[i];
            const auto incoming = next->cfg->Predecessors(info);
            if (info.function_entry || info.external_entry || info.call_return ||
                !info.has_incoming_edges || incoming.empty() ||
                std::find(incoming.begin(), incoming.end(), FunctionCFG::MissingBlock) != incoming.end()) {
                next->states[i] = FunctionInputs::Root | FunctionInputs::Reached;
                work.push_back(i);
            }
        }
        while (!work.empty()) {
            const auto id = work.back(); work.pop_back();
            for (const auto successor : next->cfg->Successors(blocks[id])) {
                if (successor != FunctionCFG::MissingBlock &&
                    !(next->states[successor] & FunctionInputs::Reached)) {
                    next->states[successor] |= FunctionInputs::Reached;
                    work.push_back(successor);
                }
            }
        }
        // Allocation failure leaves no partial graph state attached to a view.
        function_inputs = std::move(next);
        return *function_inputs;
    }

    void PrepareLabels(Input& input) {
        input.first_label = static_cast<u32>(labels.size());
        StackVector<u8, 32> bound;
        for (u32 i = 0; i < input.events.size(); ++i) {
            const auto* inst = input.events[i];
            if (inst->GetOp() != OpCode::BindLabel) continue;
            if (bound.empty()) bound.resize(input.events.size(), 0);
            const auto* source = inst->GetArg<Value>(0).Def();
            const auto end = input.events.begin() + i;
            const auto found = std::lower_bound(input.events.begin(), end, source->Id(),
                    [](const Inst* item, u32 id) { return item->Id() < id; });
            if (found == end || *found != source ||
                (source->GetOp() != OpCode::Goto && source->GetOp() != OpCode::NotGoto)) {
                input.closed = false; continue;
            }
            const auto index = static_cast<u32>(found - input.events.begin());
            if (bound[index]) { input.closed = false; continue; }
            bound[index] = 1;
            labels.push_back({i, index, point_count++});
        }
        for (u32 i = 0; i < input.events.size(); ++i) {
            const auto op = input.events[i]->GetOp();
            if ((op == OpCode::Goto || op == OpCode::NotGoto) &&
                (bound.empty() || !bound[i])) input.closed = false;
        }
        input.label_count = static_cast<u32>(labels.size()) - input.first_label;
    }

    NOINLINE bool Prove(const Block* block, const Answer& key, Value expected,
                        SemanticFacts& facts) {
        const u32 size = GetScalarIntegerBits(expected.Type()) / 8;
        std::array<LogicalByte, 8> wanted{};
        for (u32 i = 0; i < size; ++i) {
            wanted[i] = ByteOf(expected, i);
            if (wanted[i].kind == LogicalByte::Unknown) return false;
        }
        struct Probe { u32 input, end, point; u8 bytes; bool initial{}; };
        StackVector<Probe, 8> work;
        StackVector<u8, 64> visited;
        const auto start = PrepareInput(block, facts);
        const auto& first = inputs[start];
        const auto end = std::lower_bound(first.events.begin(), first.events.end(), key.before->Id(),
                [](const Inst* inst, u32 id) { return inst->Id() < id; });
        work.push_back({start, static_cast<u32>(end - first.events.begin()), 0,
                        static_cast<u8>((1u << size) - 1), true});
        while (!work.empty()) {
            auto probe = work.back(); work.pop_back();
            auto pending = probe.bytes;
            if (!probe.initial) {
                // Input/point IDs can grow during this explicit preparation.
                // Linear local proofs never allocate or clear this table.
                if (visited.size() <= probe.point)
                    visited.resize(static_cast<size_t>(probe.point) + 1, 0);
                pending &= ~visited[probe.point];
                visited[probe.point] |= pending;
            }
            if (!pending) continue;
            const auto& input = inputs[probe.input];
            if (!input.closed) return false;
            while (probe.end && pending) {
                const auto index = --probe.end;
                const auto& inst = *input.events[index];
                const auto op = inst.GetOp();
                if (op == OpCode::BindLabel) {
                    const auto begin = labels.begin() + input.first_label;
                    const auto finish = begin + input.label_count;
                    const auto label = std::lower_bound(begin, finish, index,
                            [](const Label& label, u32 i) { return label.event < i; });
                    if (label == finish || label->event != index) return false;
                    work.push_back({probe.input, label->source, label->point, pending});
                    continue;
                }
                if (op == OpCode::Goto || op == OpCode::NotGoto) continue;
                if (op == OpCode::GetHostGPR || op == OpCode::SetHostGPR) {
                    const bool read = op == OpCode::GetHostGPR;
                    if (inst.GetArg<Imm>(read ? 0 : 1).Get() != key.home) continue;
                    const auto value = read ? Value{&inst} : inst.GetArg<Value>(0);
                    const auto offset = inst.GetArg<Imm>(read ? 1 : 2).Get();
                    const auto count = GetScalarIntegerBits(value.Type()) / 8;
                    if (!count || offset > 8 || count > 8 - offset) return false;
                    for (u32 i = 0; i < size; ++i) {
                        const auto byte = key.offset + i;
                        if (!(pending & (1u << i)) || byte < offset || byte >= offset + count) continue;
                        const auto actual = ByteOf(value, static_cast<u32>(byte - offset));
                        if (actual.kind != LogicalByte::Unknown && actual == wanted[i])
                            pending &= ~(1u << i);
                        else if (!read) return false;
                    }
                } else if (!PreservesGPRBytes(inst)) {
                    return false;
                }
            }
            if (!pending) continue;
            // Crossing an origin's own entry would compare a new loop
            // iteration with an old dynamic instance of the same SSA node.
            for (u32 i = 0; i < size; ++i)
                if ((pending & (1u << i)) && wanted[i].definition &&
                    wanted[i].definition->GetBlock() == input.block) return false;
            if (policy == GuestStateEntryPolicy::Block) return false;
            const auto* input_block = input.block;
            const auto& graph = PrepareFunction(facts);
            const auto graph_id = graph.cfg->FindBlock(input_block);
            if (!graph_id || !(graph.states[*graph_id] & FunctionInputs::Reached) ||
                (graph.states[*graph_id] & FunctionInputs::Root)) return false;
            for (const auto predecessor : graph.cfg->Predecessors(graph.cfg->Blocks()[*graph_id])) {
                if (predecessor == FunctionCFG::MissingBlock) return false;
                if (!(graph.states[predecessor] & FunctionInputs::Reached)) return false;
                const auto id = PrepareInput(graph.cfg->Blocks()[predecessor].hir->GetBlock(), facts);
                work.push_back({id, static_cast<u32>(inputs[id].events.size()),
                                inputs[id].exit_point, pending});
            }
        }
        return true;
    }
};
}  // namespace

struct GuestStateFacts::VersionViews {
    std::unique_ptr<GuestStateVersions> function;
    std::vector<std::unique_ptr<GuestStateVersions>> blocks;

    const GuestStateVersions* Find(const ir::Block* block, GuestStateEntryPolicy policy) const {
        if (policy == GuestStateEntryPolicy::Function) return function.get();
        if (policy != GuestStateEntryPolicy::Block) return nullptr;
        const auto found = std::lower_bound(blocks.begin(), blocks.end(), block,
                [](const auto& view, const auto* block) {
                    return std::less<>{}(view->Scope(), block);
                });
        return found != blocks.end() && (*found)->Scope() == block ? found->get() : nullptr;
    }

    NOINLINE GuestStateVersions& Prepare(const ir::Block* block, GuestStateEntryPolicy policy) {
        if (policy == GuestStateEntryPolicy::Function) {
            if (!function) function = std::make_unique<GuestStateVersions>(block, policy);
            return *function;
        }
        const auto found = std::lower_bound(blocks.begin(), blocks.end(), block,
                [](const auto& view, const auto* block) {
                    return std::less<>{}(view->Scope(), block);
                });
        if (found != blocks.end() && (*found)->Scope() == block) return **found;
        return **blocks.insert(found, std::make_unique<GuestStateVersions>(block, policy));
    }
};

GuestStateFacts::GuestStateFacts() = default;
GuestStateFacts::~GuestStateFacts() = default;

std::optional<bool> GuestStateFacts::FindGPRMatch(
        const ir::Block* block, u32 home, u32 offset, const ir::Inst& before,
        ir::Value expected, GuestStateEntryPolicy policy) const {
    if (!version_views || home >= 32 || offset >= 8) return std::nullopt;
    if (const auto* view = version_views->Find(block, policy))
        return view->Find({&before, expected.Def(), expected.Type(),
                           static_cast<u8>(home), static_cast<u8>(offset)});
    return std::nullopt;
}

bool GuestStateFacts::PrepareGPRMatch(
        const ir::Block* block, u32 home, u32 offset, const ir::Inst& before,
        ir::Value expected, GuestStateEntryPolicy policy, SemanticFacts& facts) {
    if (!version_views) version_views = std::make_unique<VersionViews>();
    return version_views->Prepare(block, policy).Prepare(
            block, {&before, expected.Def(), expected.Type(),
                    static_cast<u8>(home), static_cast<u8>(offset)}, expected, facts);
}

bool SemanticFacts::PrepareGuestStateGPRMatch(
        const ir::Block* input, u32 home, u32 offset, const ir::Inst& before,
        ir::Value expected, GuestStateEntryPolicy policy) {
    if (!InputSealed() || before.GetBlock() != input) return false;
    // A cached key was already checked against this immutable unit. In
    // particular, do not repeat an owned-block walk or SSA preparation here.
    if (guest_state)
        if (const auto ready = guest_state->FindGPRMatch(input, home, offset, before, expected, policy))
            return *ready;
    return PrepareGuestStateGPRMatchCold(input, home, offset, before, expected, policy);
}

NOINLINE bool SemanticFacts::PrepareGuestStateGPRMatchCold(
        const ir::Block* input, u32 home, u32 offset, const ir::Inst& before,
        ir::Value expected, GuestStateEntryPolicy policy) {
    const auto width = ir::GetScalarIntegerBits(expected.Type());
    if (!InputSealed() || !Contains(input) || before.GetBlock() != input ||
        !expected.Defined() || !Contains(expected.Def()->GetBlock()) || home >= 32 ||
        !width || width % 8 || offset >= 8 || width / 8 > 8 - offset ||
        (policy != GuestStateEntryPolicy::Block && policy != GuestStateEntryPolicy::Function) ||
        (policy == GuestStateEntryPolicy::Function && !function) || !PrepareSSAValidation())
        return false;
    if (policy == GuestStateEntryPolicy::Function) {
        // Local proofs need executable membership, not construction of a CFG.
        if (function_cfg) {
            if (!function_cfg->FindBlock(input)) return false;
        } else if (std::none_of(function->GetHIRBlocksRPO().begin(),
                                function->GetHIRBlocksRPO().end(),
                                [&](const auto& member) { return member.GetBlock() == input; })) {
            return false;
        }
    }
    if (!guest_state) guest_state = std::make_unique<GuestStateFacts>();
    return guest_state->PrepareGPRMatch(input, home, offset, before, expected, policy, *this);
}

std::optional<bool> SemanticFacts::FindGuestStateGPRMatch(
        const ir::Block* input, u32 home, u32 offset, const ir::Inst& before,
        ir::Value expected, GuestStateEntryPolicy policy) const {
    if (!InputSealed() || before.GetBlock() != input)
        return std::nullopt;
    return guest_state ? guest_state->FindGPRMatch(input, home, offset, before, expected, policy)
                       : std::nullopt;
}
}  // namespace swift::runtime::compile
