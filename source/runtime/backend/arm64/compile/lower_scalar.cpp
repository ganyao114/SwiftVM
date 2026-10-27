#include "lower_scalar.h"
#include "lower_integer_flags.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/block.h"

#include <algorithm>
#include <optional>

namespace swift::runtime::backend::arm64 {

const NarrowAction* ScalarActions::Narrow(u32 id) const {
    const auto found = std::lower_bound(
            operations.begin(), operations.end(), id, [](const auto& row, u32 value) {
                return row.instruction < value;
            });
    return found != operations.end() && found->instruction == id ? &*found : nullptr;
}
bool ScalarActions::Has(u32 id, ScalarFlag flag) const {
    const auto found =
            std::lower_bound(flags.begin(), flags.end(), id, [](const auto& row, u32 value) {
                return row.instruction < value;
            });
    return found != flags.end() && found->instruction == id &&
           (static_cast<u8>(found->flags) & static_cast<u8>(flag));
}
void ScalarActions::Finish() {
    std::sort(operations.begin(), operations.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(
                   operations.begin(), operations.end(), [](const auto& a, const auto& b) {
                       return a.instruction == b.instruction;
                   }) == operations.end());
    std::sort(flags.begin(), flags.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    size_t count = 0;
    for (const auto row : flags) {
        if (count && flags[count - 1].instruction == row.instruction)
            flags[count - 1].flags = static_cast<ScalarFlag>(
                    static_cast<u8>(flags[count - 1].flags) | static_cast<u8>(row.flags));
        else
            flags[count++] = row;
    }
    flags.resize(count);
}

namespace {

constexpr u8 kMaxResolveDepth = 16;

bool Overlaps(u32 left_offset, u32 left_size, u32 right_offset, u32 right_size) {
    return left_offset < right_offset + right_size && right_offset < left_offset + left_size;
}

bool IsUniformBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::UniformBarrier:
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
        case O::CallLambda:
        case O::CallLocation:
        case O::CallDynamic:
        case O::X87Op:
        case O::Sse42Str:
            return true;
        default:
            return false;
    }
}

template <class Previous>
std::optional<ir::Value> ResolveDiv128Uniform(const ir::Inst& load, Previous&& previous) {
    const auto load_uniform = load.GetArg<ir::Uniform>(0);
    const u32 load_offset = load_uniform.GetOffset();
    const u32 load_size = ir::GetValueSizeByte(load_uniform.GetType());
    while (const ir::Inst* candidate = previous()) {
        if (IsUniformBarrier(candidate->GetOp())) return std::nullopt;
        if (candidate->GetOp() != ir::OpCode::StoreUniform) continue;
        const auto store_uniform = candidate->GetArg<ir::Uniform>(0);
        const u32 store_offset = store_uniform.GetOffset();
        const u32 store_size = ir::GetValueSizeByte(store_uniform.GetType());
        if (!Overlaps(load_offset, load_size, store_offset, store_size)) continue;
        if (load_offset != store_offset || load_size != store_size) return std::nullopt;
        return candidate->GetArg<ir::Value>(1);
    }
    return std::nullopt;
}

template <class UniformSource>
ir::Value ResolveDiv128Value(ir::Value value, const UniformSource& uniform_source) {
    u8 depth{};
    while (value.Def() && depth++ < kMaxResolveDepth) {
        const auto* definition = value.Def();
        if (definition->GetOp() == ir::OpCode::BitCast) {
            value = definition->GetArg<ir::Value>(0);
            continue;
        }
        if (definition->GetOp() == ir::OpCode::GetOperand) {
            const auto operand = definition->GetArg<ir::Operand>(0);
            if (operand.GetOp().type != ir::OperandOp::None || !operand.GetRight().Null() ||
                !operand.GetLeft().IsValue())
                break;
            value = operand.GetLeft().value;
            continue;
        }
        if (definition->GetOp() == ir::OpCode::LoadUniform) {
            const auto forwarded = uniform_source(*definition);
            if (!forwarded) break;
            value = *forwarded;
            continue;
        }
        break;
    }
    return value;
}

template <class UniformSource>
bool MatchNativeDiv128(const ir::Inst& inst, const UniformSource& uniform_source) {
    if (inst.GetOp() != ir::OpCode::Div128) return false;
    const bool sign = inst.GetArg<ir::Imm>(3).Get() != 0;
    const auto high = ResolveDiv128Value(inst.GetArg<ir::Value>(0), uniform_source);
    const ir::Inst* high_definition = high.Def();
    if (!high_definition) return false;
    if (!sign)
        return high_definition->GetOp() == ir::OpCode::LoadImm &&
               high_definition->GetArg<ir::Imm>(0).Get() == 0;
    if (high_definition->GetOp() != ir::OpCode::AsrImm ||
        high_definition->GetArg<ir::Imm>(1).Get() != 63)
        return false;
    const auto low = ResolveDiv128Value(inst.GetArg<ir::Value>(1), uniform_source);
    const auto sign_source =
            ResolveDiv128Value(high_definition->GetArg<ir::Value>(0), uniform_source);
    return sign_source.Def() && sign_source.Def() == low.Def();
}

bool Equivalent(ir::Value left, ir::Value right, u32 depth = 0) {
    if (left.Type() != right.Type() || depth == 8) {
        return false;
    }
    if (left == right) {
        return true;
    }
    auto* left_def = left.Def();
    auto* right_def = right.Def();
    if (!left_def || !right_def || left_def->GetOp() != right_def->GetOp() ||
        left_def->ReturnType() != right_def->ReturnType()) {
        return false;
    }
    if (left_def->GetOp() == ir::OpCode::BitCast) {
        return Equivalent(
                left_def->GetArg<ir::Value>(0), right_def->GetArg<ir::Value>(0), depth + 1);
    }
    if (left_def->GetOp() != ir::OpCode::BitExtract ||
        left_def->GetArg<ir::Imm>(1).Get() != right_def->GetArg<ir::Imm>(1).Get() ||
        left_def->GetArg<ir::Imm>(2).Get() != right_def->GetArg<ir::Imm>(2).Get()) {
        return false;
    }
    return Equivalent(left_def->GetArg<ir::Value>(0), right_def->GetArg<ir::Value>(0), depth + 1);
}

u32 References(const ir::Inst& user, const ir::Inst* value) {
    return std::ranges::count_if(user.GetValues(),
                                 [&](ir::Value input) { return input.Def() == value; });
}

}  // namespace

bool SelectNativeDiv128(const ir::Inst& inst, const compile::BlockAnalysisIndex& index) {
    return MatchNativeDiv128(inst, [&](const ir::Inst& load) -> std::optional<ir::Value> {
        auto position = index.InstructionPosition(&load);
        if (!position) return std::nullopt;
        const auto nodes = index.Instructions();
        return ResolveDiv128Uniform(load, [&]() -> const ir::Inst* {
            return *position ? nodes[--*position] : nullptr;
        });
    });
}

bool PrepareManualDiv128(const ir::Inst& inst, const ir::Block* block) {
    return MatchNativeDiv128(inst, [block](const ir::Inst& load) -> std::optional<ir::Value> {
        if (!block) return std::nullopt;
        const auto& instructions = block->GetInstList();
        auto position = std::find_if(instructions.begin(),
                                     instructions.end(),
                                     [&](const ir::Inst& node) { return &node == &load; });
        if (position == instructions.end()) return std::nullopt;
        return ResolveDiv128Uniform(load, [&]() -> const ir::Inst* {
            return position != instructions.begin() ? &*--position : nullptr;
        });
    });
}

ScalarActions LowerScalarCopies(const compile::BlockAnalysisIndex& index) {
    ScalarActions lowered;
    if (!index.Summary().HasScalarCopyCandidates()) return lowered;
    for (const auto* node : index.Instructions()) {
        const auto& inst = *node;
        if (inst.GetOp() != ir::OpCode::Xor) {
            continue;
        }
        const auto left = inst.GetArg<ir::Value>(0);
        const auto operand = inst.GetArg<ir::Operand>(1);
        if (operand.GetOp() != ir::OperandPlus || !operand.GetRight().Null() ||
            !operand.GetLeft().IsValue()) {
            continue;
        }
        const auto right = operand.GetLeft().value;
        if (!Equivalent(left, right)) {
            continue;
        }
        lowered.flags.push_back({inst.Id(), ScalarFlag::SelfXor});
        for (auto value : {left, right}) {
            auto* def = value.Def();
            if (def && def->GetOp() == ir::OpCode::BitExtract &&
                def->GetUses(false) == References(inst, def)) {
                if (index.Contains(def))
                    lowered.flags.push_back({def->Id(), ScalarFlag::CopyInputDiscarded});
            }
        }
    }
    lowered.Finish();
    return lowered;
}

namespace {
struct ExtractExtensionMatch {
    const ir::Inst* extract{};
    const ir::Inst* shift{};
    u8 width{};
    bool source_high_zero{};
};
struct MaskedInputMatch {
    const ir::Inst* extract{};
    u8 width{};
};
struct ShiftMaskMatch {
    const ir::Inst* shift{};
    u8 offset{};
};
bool Covered(const ScalarBitInputs& inputs, const ir::Inst* inst) {
    return inst && inputs.earlier_coverage && inst->Id() < inputs.earlier_coverage->size() &&
           inputs.earlier_coverage->test(inst->Id());
}

// Producers whose host emission writes the W form for a <=32-bit result,
// zeroing bits [63:32] of the destination register. The ALU set mirrors
// the width verifier's IsPinnedWViewProducer; loads and zero extensions
// have the same property by their emission contract.
bool ProducesZeroUpper32(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::LoadUniform:
        case ir::OpCode::ZeroExtend32:
        case ir::OpCode::ZeroExtend32To64:
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
        case ir::OpCode::And:
        case ir::OpCode::AndNot:
        case ir::OpCode::Or:
        case ir::OpCode::Xor:
        case ir::OpCode::Mul:
        case ir::OpCode::LslImm:
        case ir::OpCode::LsrImm:
        case ir::OpCode::AsrImm:
        case ir::OpCode::RorImm:
        case ir::OpCode::RorValue:
        case ir::OpCode::Neg:
        case ir::OpCode::LoadImm:
        case ir::OpCode::Select:
        case ir::OpCode::SelectZero:
        case ir::OpCode::CondSelect:
        case ir::OpCode::PopCount:
            return true;
        default:
            return false;
    }
}

bool IsNarrowLoadZeroExtended(ir::Value value, u32 width) {
    for (u32 depth = 0; depth != 8 && value.Defined(); ++depth) {
        auto* definition = value.Def();
        while (definition && definition->IsBitCastOperation()) {
            value = definition->GetArg<ir::Value>(0);
            definition = value.Def();
        }
        if (!definition) {
            return false;
        }
        if (definition->GetOp() == ir::OpCode::LoadMemory ||
            definition->GetOp() == ir::OpCode::LoadMemoryTSO ||
            definition->GetOp() == ir::OpCode::LoadUniform) {
            return ir::GetValueSizeByte(definition->ReturnType()) <= width;
        }
        if (definition->GetOp() != ir::OpCode::ZeroExtend32 &&
            definition->GetOp() != ir::OpCode::ZeroExtend32To64) {
            return false;
        }
        value = definition->GetArg<ir::Value>(0);
    }
    return false;
}

}  // namespace

class NarrowBitBuilder {
public:
    NarrowBitBuilder(const compile::BlockAnalysisIndex& analysis,
                     compile::CompileWorkspace& workspace,
                     compile::AllocationView allocation,
                     const ScalarBitInputs& inputs)
            : analysis(analysis), allocation(allocation), inputs(inputs), candidates(workspace) {}
    ScalarActions Build();

private:
    enum Role : u16 { Extension = 1, Mask = 2, Tied = 4, Shift = 8, Extended = 16, Masked = 32 };
    void SeedCandidates();
    compile::LoweringCandidate* FindCandidate(const ir::Inst*);
    bool CopyDiscarded(const ir::Inst*) const;
    bool BlockedExtract(const ir::Inst*) const;
    bool Flagged(const ir::Inst*) const;
    std::optional<ExtractExtensionMatch> MatchExtension(size_t) const;
    std::optional<MaskedInputMatch> MatchMask(size_t) const;
    std::optional<ShiftMaskMatch> MatchShiftMask(size_t);
    const compile::BlockAnalysisIndex& analysis;
    compile::AllocationView allocation;
    const ScalarBitInputs& inputs;
    ScalarActions lowered;
    compile::LoweringCandidateScope candidates;
};

void NarrowBitBuilder::SeedCandidates() {
    const auto nodes = analysis.Instructions();
    ASSERT(nodes.size() <= size_t{UINT16_MAX} + 1);
    for (size_t position = 0; position < nodes.size(); ++position) {
        u16 roles{};
        switch (nodes[position]->GetOp()) {
            case ir::OpCode::ZeroExtend32:
                roles = Extension | Tied;
                break;
            case ir::OpCode::ZeroExtend32To64:
                roles = Tied;
                break;
            case ir::OpCode::And:
                roles = Mask;
                break;
            case ir::OpCode::LsrImm:
            case ir::OpCode::AsrImm:
                roles = Shift;
                break;
            default:
                continue;
        }
        candidates.Entries().push_back({static_cast<u16>(position), roles});
    }
}

compile::LoweringCandidate* NarrowBitBuilder::FindCandidate(const ir::Inst* inst) {
    const auto position = analysis.InstructionPosition(inst);
    if (!position) return nullptr;
    auto& entries = candidates.Entries();
    const auto found = std::lower_bound(
            entries.begin(), entries.end(), *position, [](const auto& row, size_t value) {
                return row.position < value;
            });
    return found != entries.end() && found->position == *position ? &*found : nullptr;
}

// Matching passes only roster nodes or their verified adjacent definitions.
bool NarrowBitBuilder::CopyDiscarded(const ir::Inst* inst) const {
    return inputs.copies && inputs.copies->Has(inst->Id(), ScalarFlag::CopyInputDiscarded);
}
bool NarrowBitBuilder::BlockedExtract(const ir::Inst* inst) const {
    return allocation.GPRLowView(inst->Id()) ||
           (inputs.integer_flags && inputs.integer_flags->Input(inst->Id())) ||
           allocation.IsWidthChainCoalesced(inst->Id()) ||
           allocation.IsLow32CopyCoalesced(inst->Id()) || CopyDiscarded(inst);
}
bool NarrowBitBuilder::Flagged(const ir::Inst* inst) const {
    return (inputs.branch && inputs.branch->producer == inst->Id()) ||
           !analysis.PseudoFlags().For(inst).Null();
}

std::optional<ExtractExtensionMatch> NarrowBitBuilder::MatchExtension(size_t position) const {
    const auto nodes = analysis.Instructions();
    const auto* wrapper = nodes[position];
    if (wrapper->GetOp() != ir::OpCode::ZeroExtend32) {
        return std::nullopt;
    }
    auto value = wrapper->GetArg<ir::Value>(0);
    auto* extract = value.Def();
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        extract->GetArg<ir::Imm>(1).Get() != 0 || extract->GetUses() != 1 ||
        extract->GetUses(false) != 1 || extract->GetArg<ir::Value>(0).Def() == nullptr ||
        allocation.IsWidthChainCoalesced(wrapper->Id())) {
        return std::nullopt;
    }
    // Adjacency proves this exact definition belongs to the input block,
    // before querying the earlier selections by numeric ID.
    if (!position || nodes[position - 1] != extract || BlockedExtract(extract))
        return std::nullopt;
    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if ((width != sizeof(u8) && width != sizeof(u16)) ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8 ||
        ir::GetValueSizeByte(value.Type()) != width ||
        allocation.ValueType(extract->GetArg<ir::Value>(0)) == compile::AllocationView::MEM) {
        // A spill reload region may end at the extract and never write its
        // slot. A later wrapper cannot extend that region in the emitter.
        // The IR width pass handles this fusion before allocation instead.
        return std::nullopt;
    }
    const ir::Inst* shift{};
    const auto* after_wrapper = position + 1 < nodes.size() ? nodes[position + 1] : nullptr;
    if (wrapper->GetUses() == 1 && wrapper->GetUses(false) == 1 && after_wrapper &&
        after_wrapper->GetOp() == ir::OpCode::LsrImm &&
        after_wrapper->GetArg<ir::Value>(0).Def() == wrapper) {
        const u64 amount = after_wrapper->GetArg<ir::Imm>(1).Get();
        if (amount > 0 && amount < width * 8) {
            shift = after_wrapper;
        }
    }
    return ExtractExtensionMatch{
            .extract = extract,
            .shift = shift,
            .width = static_cast<u8>(width),
            .source_high_zero = IsNarrowLoadZeroExtended(extract->GetArg<ir::Value>(0), width),
    };
}

std::optional<MaskedInputMatch> NarrowBitBuilder::MatchMask(size_t position) const {
    const auto nodes = analysis.Instructions();
    const auto* consumer = nodes[position];
    if (consumer->GetOp() != ir::OpCode::And) {
        return std::nullopt;
    }
    auto left = consumer->GetArg<ir::Value>(0);
    auto* extract = left.Def();
    const u32 width = ir::GetValueSizeByte(consumer->ReturnType());
    // Allocation keeps the parent live only through this adjacent extract.
    // The same pointer check establishes ownership for the earlier ID gates.
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        (width != sizeof(u8) && width != sizeof(u16)) ||
        ir::GetValueSizeByte(left.Type()) != width || extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8 || extract->GetUses() != 1 ||
        extract->GetUses(false) != 1 || !extract->GetArg<ir::Value>(0).Defined() ||
        !position || nodes[position - 1] != extract || BlockedExtract(extract) ||
        allocation.ValueType(extract->GetArg<ir::Value>(0)) == compile::AllocationView::MEM) {
        return std::nullopt;
    }
    const auto right = consumer->GetArg<ir::Operand>(1);
    u64 mask{};
    if (right.IsImm()) {
        mask = right.GetLeft().imm.Get();
    } else if (right.GetLeft().IsValue() && right.GetRight().Null()) {
        auto* definition = right.GetLeft().value.Def();
        if (!definition || definition->GetOp() != ir::OpCode::LoadImm) {
            return std::nullopt;
        }
        mask = definition->GetArg<ir::Imm>(0).Get();
    } else {
        return std::nullopt;
    }
    if ((mask >> (width * 8)) != 0) {
        return std::nullopt;
    }
    return MaskedInputMatch{
            .extract = extract,
            .width = static_cast<u8>(width),
    };
}

std::optional<ShiftMaskMatch> NarrowBitBuilder::MatchShiftMask(size_t position) {
    const auto nodes = analysis.Instructions();
    const auto* consumer = nodes[position];
    if (consumer->GetOp() != ir::OpCode::And || Flagged(consumer)) {
        return std::nullopt;
    }
    const auto right = consumer->GetArg<ir::Operand>(1);
    if (!right.IsImm() || right.GetLeft().imm.Get() != 1) {
        return std::nullopt;
    }
    const auto value = consumer->GetArg<ir::Value>(0);
    auto* shift = value.Def();
    if (!shift || (shift->GetOp() != ir::OpCode::LsrImm && shift->GetOp() != ir::OpCode::AsrImm) ||
        !position || nodes[position - 1] != shift || Flagged(shift) ||
        shift->GetUses() != 1 || shift->GetUses(false) != 1 ||
        ir::GetValueSizeByte(value.Type()) != ir::GetValueSizeByte(consumer->ReturnType()) ||
        CopyDiscarded(shift)) {
        return std::nullopt;
    }
    const auto* selected = FindCandidate(shift);
    ASSERT(selected && (selected->roles & Shift));
    if (selected->roles & Extended) return std::nullopt;
    const auto source = shift->GetArg<ir::Value>(0);
    const u32 width = ir::GetValueSizeByte(source.Type()) * 8;
    const u64 offset = shift->GetArg<ir::Imm>(1).Get();
    if (!source.Defined() || width != ir::GetValueSizeByte(value.Type()) * 8 || offset >= width ||
        allocation.ValueType(source) == compile::AllocationView::NONE ||
        allocation.ValueType(source) == compile::AllocationView::MEM) {
        return std::nullopt;
    }
    return ShiftMaskMatch{
            .shift = shift,
            .offset = static_cast<u8>(offset),
    };
}

ScalarActions NarrowBitBuilder::Build() {
    SeedCandidates();
    const auto nodes = analysis.Instructions();
    for (const auto& entry : candidates.Entries()) {
        if (!(entry.roles & Extension)) continue;
        const auto& inst = *nodes[entry.position];
        const auto candidate = MatchExtension(entry.position);
        if (!candidate) continue;
        if (Covered(inputs, &inst) || Covered(inputs, candidate->extract) ||
            Covered(inputs, candidate->shift))
            continue;
        lowered.flags.push_back({candidate->extract->Id(), ScalarFlag::BitInputCovered});
        const auto kind = candidate->shift ? NarrowAction::DeferredExtend : NarrowAction::Extend;
        lowered.operations.push_back(
                {inst.Id(),
                 kind,
                 static_cast<u8>(candidate->width | (candidate->source_high_zero ? 0x80 : 0))});
        if (candidate->shift) {
            auto* selected = FindCandidate(candidate->shift);
            ASSERT(selected && (selected->roles & Shift));
            selected->roles |= Extended;
            lowered.operations.push_back(
                    {candidate->shift->Id(), NarrowAction::ExtractShift, candidate->width});
        }
    }
    for (const auto& entry : candidates.Entries()) {
        if (!(entry.roles & Mask)) continue;
        const auto& inst = *nodes[entry.position];
        const auto candidate = MatchMask(entry.position);
        if (!candidate) continue;
        if (Covered(inputs, &inst) || Covered(inputs, candidate->extract)) continue;
        lowered.operations.push_back({inst.Id(), NarrowAction::Mask, candidate->width});
        lowered.flags.push_back({candidate->extract->Id(), ScalarFlag::BitInputCovered});
    }
    for (const auto& entry : candidates.Entries()) {
        if (!(entry.roles & Mask)) continue;
        const auto& inst = *nodes[entry.position];
        const auto candidate = MatchShiftMask(entry.position);
        if (!candidate) continue;
        if (Covered(inputs, &inst) || Covered(inputs, candidate->shift)) continue;
        lowered.operations.push_back({inst.Id(), NarrowAction::ShiftMask, candidate->offset});
        lowered.flags.push_back({candidate->shift->Id(), ScalarFlag::BitInputCovered});
        auto* selected = FindCandidate(candidate->shift);
        ASSERT(selected && (selected->roles & Shift));
        selected->roles |= Masked;
    }
    for (const auto& entry : candidates.Entries()) {
        if (!(entry.roles & Tied)) continue;
        const auto& inst = *nodes[entry.position];
        if (allocation.IsWidthChainCoalesced(inst.Id())) continue;
        auto source = inst.GetArg<ir::Value>(0);
        auto* definition = source.Def();
        while (definition && definition->IsBitCastOperation()) {
            source = definition->GetArg<ir::Value>(0);
            definition = source.Def();
        }
        if (!definition || !ProducesZeroUpper32(definition->GetOp())) continue;
        // Preserve the wide-emission, low preserved view and masked-shift exclusions.
        const auto* selected = FindCandidate(definition);
        if (allocation.IsWidthChainCoalesced(definition->Id()) ||
            allocation.PreservedGPRDefinitionAt(definition->Id()) ||
            (selected && (selected->roles & Masked)))
            continue;
        if (ir::GetValueSizeByte(definition->ReturnType()) > ir::GetValueSizeByte(source.Type()))
            continue;
        if (allocation.ValueType(source) != compile::AllocationView::GPR ||
            allocation.ValueType(ir::Value{&inst}) != compile::AllocationView::GPR ||
            allocation.ValueGPR(source).id != allocation.ValueGPR(ir::Value{&inst}).id ||
            allocation.SpillReloadAt(source, inst.Id()))
            continue;
        lowered.flags.push_back({inst.Id(), ScalarFlag::TiedZeroExtend});
    }
    lowered.Finish();
    return std::move(lowered);
}

ScalarActions LowerNarrowBits(const compile::BlockAnalysisIndex& analysis,
                              compile::CompileWorkspace& workspace,
                              compile::AllocationView allocation,
                              const ScalarBitInputs& inputs) {
    if (!analysis.Summary().HasNarrowBitCandidates()) return {};
    return NarrowBitBuilder{analysis, workspace, allocation, inputs}.Build();
}
}  // namespace swift::runtime::backend::arm64
