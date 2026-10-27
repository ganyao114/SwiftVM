#include "register_state.h"
#include "runtime/backend/arm64/compile/register_contract.h"

#include <algorithm>
#include <vector>

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

using ExtensionFacts = RegisterStatePreparation::ExtensionFacts;
using FunctionWidthFacts = std::array<ExtensionFacts, 30>;

FunctionWidthFacts TopWidthFacts() {
    FunctionWidthFacts facts{};
    for (u32 home = 0; home < facts.size(); ++home) {
        if (IsFixedGPRHome(home)) {
            facts[home] = {
                    .known_zero_above = 1,
                    .sign_extended_from = 1,
                    .sign_extended_to = 64,
            };
        }
    }
    return facts;
}

ExtensionFacts Meet(ExtensionFacts left, ExtensionFacts right) {
    ExtensionFacts result{};
    if (left.known_zero_above && right.known_zero_above) {
        result.known_zero_above =
                std::max(left.known_zero_above, right.known_zero_above);
    }
    if (left.sign_extended_from && right.sign_extended_from) {
        result.sign_extended_from =
                std::max(left.sign_extended_from, right.sign_extended_from);
        result.sign_extended_to =
                std::min(left.sign_extended_to, right.sign_extended_to);
        if (result.sign_extended_from >= result.sign_extended_to) {
            result.sign_extended_from = 0;
            result.sign_extended_to = 0;
        }
    }
    return result;
}

ExtensionFacts DefinitionExtensionFacts(
        ir::Value value,
        const FunctionWidthFacts& entry_facts) {
    if (!value.Def()) {
        return {};
    }
    const u32 width = ir::GetValueSizeByte(value.Type());
    auto* definition = value.Def();
    switch (definition->GetOp()) {
        case ir::OpCode::ZeroExtend32: {
            const u32 source_bits = ir::GetValueSizeByte(
                    definition->GetArg<ir::Value>(0).Type()) * 8;
            return {.known_zero_above = static_cast<u8>(source_bits)};
        }
        case ir::OpCode::ZeroExtend32To64: {
            auto facts = DefinitionExtensionFacts(
                    definition->GetArg<ir::Value>(0), entry_facts);
            if (!facts.known_zero_above || facts.known_zero_above > 32) {
                facts.known_zero_above = 32;
            }
            return facts;
        }
        case ir::OpCode::SignExtend: {
            const u32 source_bits = ir::GetValueSizeByte(
                    definition->GetArg<ir::Value>(0).Type()) * 8;
            return {
                    .sign_extended_from = static_cast<u8>(source_bits),
                    .sign_extended_to = static_cast<u8>(width * 8),
            };
        }
        case ir::OpCode::LoadImm: {
            const u64 immediate = definition->GetArg<ir::Imm>(0).Get();
            const u8 bits = immediate <= UINT8_MAX
                    ? 8
                    : immediate <= UINT16_MAX
                            ? 16
                            : immediate <= UINT32_MAX ? 32 : 0;
            return {.known_zero_above = bits};
        }
        case ir::OpCode::GetHostGPR: {
            const u32 home = definition->GetArg<ir::Imm>(0).Get();
            return definition->GetArg<ir::Imm>(1).Get() == 0 &&
                           home < entry_facts.size()
                    ? entry_facts[home]
                    : ExtensionFacts{};
        }
        default:
            if (definition->IsBitCastOperation()) {
                return DefinitionExtensionFacts(
                        definition->GetArg<ir::Value>(0), entry_facts);
            }
            return {};
    }
}

ExtensionFacts PublicationExtensionFacts(
        ir::Value value,
        const FunctionWidthFacts& entry_facts) {
    auto facts = DefinitionExtensionFacts(value, entry_facts);
    const u32 width = ir::GetValueSizeByte(value.Type());
    if (width == sizeof(u32)) {
        if (!facts.known_zero_above || facts.known_zero_above > 32) {
            facts.known_zero_above = 32;
        }
        if (facts.sign_extended_to > 32) {
            facts.sign_extended_to = 32;
        }
        if (facts.sign_extended_from >= facts.sign_extended_to) {
            facts.sign_extended_from = 0;
            facts.sign_extended_to = 0;
        }
    }
    return facts;
}

}  // namespace

void RegisterStatePreparation::BeginFunction(const ir::HIRFunction& next_function,
                                    const FeatureSet& next_features,
                                    compile::SemanticFacts& facts) {
    ASSERT(facts.MatchesInput(nullptr, &next_function));
    function = &next_function;
    semantic_facts = &facts;
    features = next_features;
    ResetGPRWindows();
    successful_survival_scans = 0;
    function_width_facts_ready = false;
    function_entry_width_facts.clear();
    block_entry_width_facts = {};
}

void RegisterStatePreparation::BuildFunctionWidthFacts() {
    if (function_width_facts_ready || !function) {
        return;
    }
    const auto top = TopWidthFacts();
    const auto& graph = semantic_facts->PrepareFunctionCFG();
    const auto blocks = graph.Blocks();
    const auto is_root = [](const auto& block) {
        return block.function_entry || block.external_entry || !block.has_incoming_edges ||
               block.call_return;
    };
    function_entry_width_facts.assign(blocks.size(), top);
    for (size_t i = 0; i < blocks.size(); ++i)
        if (is_root(blocks[i])) function_entry_width_facts[i] = {};
    std::vector<WidthFacts> exit_facts(blocks.size(), top);

    auto transfer = [&](const ir::HIRBlock* hir_block, WidthFacts facts) {
        for (auto& inst : hir_block->GetBlock()->GetInstList()) {
            if (inst.GetOp() == ir::OpCode::SetHostGPR) {
                const u32 home = inst.GetArg<ir::Imm>(1).Get();
                if (!IsFixedGPRHome(home)) {
                    continue;
                }
                const u32 offset = inst.GetArg<ir::Imm>(2).Get();
                const auto value = inst.GetArg<ir::Value>(0);
                const u32 width = ir::GetValueSizeByte(value.Type());
                if (offset == 0 &&
                    (width == sizeof(u32) || width == sizeof(u64))) {
                    facts[home] = PublicationExtensionFacts(value, facts);
                } else if (offset + width > sizeof(u32)) {
                    facts[home] = {};
                } else {
                    const u8 end_bits = static_cast<u8>((offset + width) * 8);
                    if (facts[home].known_zero_above &&
                        facts[home].known_zero_above < end_bits) {
                        facts[home].known_zero_above = end_bits;
                    }
                    facts[home].sign_extended_from = 0;
                    facts[home].sign_extended_to = 0;
                }
                continue;
            }

            const auto helper = HelperCallContract::Resolve(inst, features);
            if (helper) {
                for (u32 home = 0; home < facts.size(); ++home) {
                    if (IsFixedGPRHome(home) && home <= 9 && helper->ClobbersGPR(home)) {
                        facts[home] = {};
                    }
                }
            } else if (inst.GetOp() == ir::OpCode::CallLambda ||
                       inst.GetOp() == ir::OpCode::CallLocation ||
                       inst.GetOp() == ir::OpCode::CallDynamic) {
                for (u32 home = 0; home < facts.size(); ++home) {
                    if (IsFixedGPRHome(home)) {
                        facts[home] = {};
                    }
                }
            }
        }
        return facts;
    };

    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto& block = blocks[i];
            WidthFacts incoming = top;
            if (is_root(block)) {
                incoming = {};
            } else {
                for (const auto predecessor : graph.Predecessors(block)) {
                    if (predecessor == compile::FunctionCFG::MissingBlock) {
                        incoming = {};
                        break;
                    }
                    for (u32 home = 0; home < incoming.size(); ++home) {
                        incoming[home] = Meet(incoming[home], exit_facts[predecessor][home]);
                    }
                }
            }

            if (function_entry_width_facts[i] != incoming) {
                function_entry_width_facts[i] = incoming;
                changed = true;
            }
            const auto outgoing = transfer(block.hir, incoming);
            if (exit_facts[i] != outgoing) {
                exit_facts[i] = outgoing;
                changed = true;
            }
        }
    } while (changed);
    function_width_facts_ready = true;
}

void RegisterStatePreparation::PrepareCurrentEntryWidthFacts(bool fault_capture_needed) {
    if (!function || !block || function_width_facts_ready) {
        return;
    }
    bool needed = fault_capture_needed;
    if (!needed) {
        for (const auto& publication : block->GetInstList()) {
            if (publication.GetOp() != ir::OpCode::SetHostGPR ||
                publication.GetArg<ir::Imm>(2).Get() != 0) {
                continue;
            }
            const auto value = publication.GetArg<ir::Value>(0);
            auto* definition = value.Def();
            needed = definition && value.Type() == ir::ValueType::U32 &&
                    definition->GetOp() == ir::OpCode::GetHostGPR &&
                    definition->GetArg<ir::Imm>(1).Get() == 0 &&
                    definition->GetArg<ir::Imm>(0).Get() ==
                            publication.GetArg<ir::Imm>(1).Get();
            if (needed) {
                break;
            }
        }
    }
    if (!needed) {
        return;
    }
    BuildFunctionWidthFacts();
    const auto id = semantic_facts->FindFunctionCFG()->FindBlock(block);
    if (id) block_entry_width_facts = function_entry_width_facts[*id];
}

RegisterStatePreparation::ExtensionFacts RegisterStatePreparation::PrepareEntryExtensionFacts(
        const ir::Block* query_block,
        u32 home) {
    if (!query_block || home >= block_entry_width_facts.size()) {
        return {};
    }
    BuildFunctionWidthFacts();
    if (!function) return {};
    const auto id = semantic_facts->FindFunctionCFG()->FindBlock(query_block);
    return id ? function_entry_width_facts[*id][home] : ExtensionFacts{};
}

bool RegisterStatePreparation::HasEntryWidthFacts(const ir::Block* current) const {
    if (!function_width_facts_ready) return false;
    const auto id = semantic_facts->FindFunctionCFG()->FindBlock(current);
    return id && std::any_of(function_entry_width_facts[*id].begin(),
                             function_entry_width_facts[*id].end(),
                             [](ExtensionFacts facts) { return facts != ExtensionFacts{}; });
}

}  // namespace swift::runtime::backend::arm64
