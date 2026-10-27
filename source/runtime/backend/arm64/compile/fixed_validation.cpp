#include "fixed_validation.h"

#include <algorithm>
#include <functional>
#include "register_contract.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/compile/allocate/writer.h"
#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::backend::arm64 {
namespace {
bool IsExtension(ir::OpCode op) {
    return op == ir::OpCode::ZeroExtend32To64 || op == ir::OpCode::ZeroExtend32 ||
           op == ir::OpCode::ZeroExtend64 || op == ir::OpCode::SignExtend;
}
}  // namespace
u64 FixedValidationContractKey(const FeatureSet& features) {
    const auto& config = GetSvmConfig();
    return (static_cast<u64>(config.x86_pin_ext) << 32) | static_cast<u64>(config.static_regs) |
           (static_cast<u64>(!!config.exec_prof) << 1) |
           (static_cast<u64>(ScratchXPoolEnabled(features)) << 2);
}

FixedValidationBuilder::FixedValidationBuilder(const FeatureSet& features)
        : features(features), data(std::make_unique<compile::FixedValidationInputs>()) {
    data->contract_key = FixedValidationContractKey(features);
}
void FixedValidationBuilder::BeginBlock(const ir::Block* block,
                                        const compile::BlockAnalysisIndex& index) {
    data->blocks.push_back({block, &index});
}
void FixedValidationBuilder::DescribeSteps(const ir::Inst* instruction) {
    bool first = true;
    while (instruction) {
        if (instruction->IsBitCastOperation()) {
            if (!described_steps.insert(instruction).second) break;
            instruction = instruction->GetArg<ir::Value>(0).Def();
            first = false;
            continue;
        }
        const bool parent = IsExtension(instruction->GetOp());
        if (!parent && !(first && instruction->GetOp() == ir::OpCode::SetHostGPR)) break;
        if (!described_steps.insert(instruction).second) break;
        const auto source = instruction->GetArg<ir::Value>(0);
        data->steps.push_back({instruction,
                               source.Def(),
                               source.Defined() ? source.Type() : ir::ValueType{},
                               parent});
        instruction = source.Def();
        first = false;
    }
}
void FixedValidationBuilder::Describe(const ir::Inst& instruction) {
    using Inputs = compile::FixedValidationInputs;
    const auto op = instruction.GetOp();
    u32 mask = FixedGPRClobbers(instruction, features);
    if (const auto helper = HelperCallContract::Resolve(instruction, features))
        for (u32 home = 0; home <= 9; ++home)
            if (helper->ClobbersGPR(home)) mask |= 1u << home;
    u8 flags = 0, home = UINT8_MAX;
    if (op == ir::OpCode::ZeroExtend32To64 || op == ir::OpCode::BitExtract ||
        op == ir::OpCode::SignExtend)
        flags |= Inputs::WidthAlias;
    if (op == ir::OpCode::GetHostGPR) {
        flags |= Inputs::HostRead;
        if (ir::GetValueSizeByte(instruction.ReturnType()) == 8) flags |= Inputs::FullRead;
        home = instruction.GetArg<ir::Imm>(0).Get();
    }
    if (op == ir::OpCode::SetHostGPR) {
        flags |= Inputs::HostWrite;
        home = instruction.GetArg<ir::Imm>(1).Get();
    }
    if (op == ir::OpCode::LoadImm && (instruction.GetArg<ir::Imm>(0).Get() >> 32) == 0)
        flags |= Inputs::NarrowConstant;
    if (flags || mask) data->sites.push_back({mask, instruction.Id(), flags, home});
    if (IsExtension(op) || op == ir::OpCode::SetHostGPR) DescribeSteps(&instruction);
}
std::unique_ptr<const compile::FixedValidationInputs> FixedValidationBuilder::Finish() {
    std::sort(data->sites.begin(), data->sites.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    std::sort(data->steps.begin(), data->steps.end(), [](const auto& a, const auto& b) {
        return std::less<const ir::Inst*>{}(a.instruction, b.instruction);
    });
    std::sort(data->blocks.begin(), data->blocks.end(), [](const auto& a, const auto& b) {
        return std::less<const ir::Block*>{}(a.block, b.block);
    });
    return std::move(data);
}
FixedValidationDescription::FixedValidationDescription(const ir::Block* block,
                                                       const compile::AllocationWriter& allocation,
                                                       const FeatureSet& features,
                                                       const compile::BlockAnalysisIndex& index) {
    if (const auto* prepared = allocation.PreparedFixedValidation(
                features, FixedValidationContractKey(features))) {
        const auto* range = prepared->BlockFor(block);
        ASSERT((range && range->index == &index) || block->GetInstList().empty());
        data = prepared;
        return;
    }
    FixedValidationBuilder builder{features};
    builder.BeginBlock(block, index);
    for (const auto* inst : index.Instructions()) builder.Describe(*inst);
    owned = builder.Finish();
    data = owned.get();
}
}  // namespace swift::runtime::backend::arm64
