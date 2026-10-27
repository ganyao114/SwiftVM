#include "spill_inputs.h"

#include <algorithm>
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/facts.h"

namespace swift::runtime::backend::arm64 {
using namespace ir;
namespace {
bool IsLocalControlFlow(OpCode op) {
    return op == OpCode::BindLabel || op == OpCode::Goto || op == OpCode::NotGoto;
}

bool SupportsDefinitionTransfer(OpCode op) {
    using O = OpCode;
    switch (op) {
        case O::LoadImm:
        case O::LoadMemory:
        case O::GetHostGPR:
        case O::Add:
        case O::Sub:
        case O::Adc:
        case O::Sbb:
        case O::Mul:
        case O::MulSub:
        case O::And:
        case O::Or:
        case O::Xor:
        case O::Not:
        case O::Select:
        case O::SelectZero:
        case O::CondSelect:
        case O::LslImm:
        case O::LsrImm:
        case O::AsrImm:
        case O::LslValue:
        case O::LsrValue:
        case O::AsrValue:
        case O::BitExtract:
        case O::Sse42Str:
            return true;
        default:
            return false;
    }
}

bool SupportsDefinitionConsumer(OpCode op) {
    using O = OpCode;
    switch (op) {
        case O::StoreUniform:
        case O::StoreMemory:
        case O::StoreMemoryTSO:
        case O::Add:
        case O::Sub:
        case O::Adc:
        case O::Sbb:
        case O::Mul:
        case O::MulSub:
        case O::And:
        case O::Or:
        case O::Xor:
        case O::Not:
        case O::Select:
        case O::SelectZero:
        case O::CondSelect:
        case O::LslImm:
        case O::LsrImm:
        case O::AsrImm:
        case O::LslValue:
        case O::LsrValue:
        case O::AsrValue:
        case O::BitExtract:
        case O::PublishSse42StrFlags:
            return true;
        default:
            return false;
    }
}

bool SupportsPendingWriteHandoff(OpCode op) {
    return SupportsDefinitionConsumer(op) || op == OpCode::SetHostGPR;
}

}  // namespace

SpillInputBuilder::SpillInputBuilder(const ir::Block* block,
                                     const compile::BlockAnalysisIndex& uses,
                                     std::unique_ptr<compile::SpillInputs>& data)
        : block(block), uses(uses), data(data) {}
void SpillInputBuilder::Describe(const Inst& inst, u32 position, bool integer_operands) {
    using Inputs = compile::SpillInputs;
    const auto op = inst.GetOp();
    if (IsLocalControlFlow(op)) {
        if (position > segment_start) segments.push_back({segment_start, position - segment_start});
        segment_start = position + 1;
        return;
    }
    u8 flags = 0;
    if (SupportsDefinitionTransfer(op)) flags |= Inputs::DefinitionTransfer;
    if (SupportsDefinitionConsumer(op)) flags |= Inputs::DefinitionConsumer;
    if (SupportsPendingWriteHandoff(op)) flags |= Inputs::PendingHandoff;
    if (op == OpCode::GetOperand) flags |= Inputs::OwnershipInput;
    if (op == OpCode::LoadMemory) flags |= Inputs::OwnershipResult;
    if (flags) sites.push_back({inst.Id(), flags});
    has_integer_uses |= integer_operands;
}
void SpillInputBuilder::Finish() {
    const u32 end = uses.Instructions().size();
    if (end > segment_start) segments.push_back({segment_start, end - segment_start});
    // Without a non-pseudo scalar operand, the physical use projection is
    // empty for every allocation attempt, so retain no block payload.
    if (!has_integer_uses || segments.empty()) return;
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    if (!data) data = std::make_unique<compile::SpillInputs>();
    const u32 first_site = data->sites.size(), first_segment = data->segments.size();
    data->sites.insert(data->sites.end(), sites.begin(), sites.end());
    data->segments.insert(data->segments.end(), segments.begin(), segments.end());
    data->blocks.push_back({block,
                            first_site,
                            static_cast<u32>(sites.size()),
                            first_segment,
                            static_cast<u32>(segments.size())});
}
}  // namespace swift::runtime::backend::arm64
