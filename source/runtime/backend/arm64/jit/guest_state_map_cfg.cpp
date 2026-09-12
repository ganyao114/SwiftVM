#include "runtime/backend/reg_alloc.h"
#include "guest_state_map.h"

#include <algorithm>
#include <vector>

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {

namespace {

using ::swift::runtime::backend::IsFixedGPRHome;

using ExtensionFacts = GuestStateMap::ExtensionFacts;
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

void GuestStateMap::AnalyzeFunction(ir::HIRFunction* function,
                                    const FeatureSet& next_features) {
    this->function = function;
    features = next_features;
    function_width_facts_ready = false;
    function_entry_width_facts.clear();
    block_entry_width_facts = {};
}

void GuestStateMap::BuildFunctionWidthFacts() {
    if (function_width_facts_ready || !function) {
        return;
    }
    function_width_facts_ready = true;

    const auto top = TopWidthFacts();
    std::unordered_set<const ir::HIRBlock*> roots;
    if (auto* entry = function->GetEntryBlock()) {
        roots.insert(entry);
    }
    for (auto* root : function->GetExternalEntryRoots()) {
        roots.insert(root);
    }

    std::vector<ir::HIRBlock*> blocks;
    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        blocks.push_back(&hir_block);
        if (!hir_block.HasIncomingEdges() || hir_block.IsCallReturnBlock()) {
            roots.insert(&hir_block);
        }
        function_entry_width_facts.emplace(hir_block.GetBlock(),
                                           roots.contains(&hir_block)
                                                   ? WidthFacts{}
                                                   : top);
    }

    std::unordered_map<const ir::HIRBlock*, WidthFacts> exit_facts;
    for (auto* hir_block : blocks) {
        exit_facts.emplace(hir_block, top);
    }

    auto transfer = [&](ir::HIRBlock* hir_block, WidthFacts facts) {
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
        for (auto* hir_block : blocks) {
            WidthFacts incoming = top;
            if (roots.contains(hir_block)) {
                incoming = {};
            } else {
                for (auto* predecessor : hir_block->GetPredecessors()) {
                    const auto found = exit_facts.find(predecessor);
                    if (found == exit_facts.end()) {
                        incoming = {};
                        break;
                    }
                    for (u32 home = 0; home < incoming.size(); ++home) {
                        incoming[home] = Meet(incoming[home], found->second[home]);
                    }
                }
            }

            auto* ir_block = hir_block->GetBlock();
            if (function_entry_width_facts[ir_block] != incoming) {
                function_entry_width_facts[ir_block] = incoming;
                changed = true;
            }
            const auto outgoing = transfer(hir_block, incoming);
            if (exit_facts[hir_block] != outgoing) {
                exit_facts[hir_block] = outgoing;
                changed = true;
            }
        }
    } while (changed);
}

void GuestStateMap::PrepareCurrentEntryWidthFacts(bool fault_capture_needed) {
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
    if (const auto found = function_entry_width_facts.find(block);
        found != function_entry_width_facts.end()) {
        block_entry_width_facts = found->second;
    }
}

GuestStateMap::ExtensionFacts GuestStateMap::EntryExtensionFacts(
        const ir::Block* query_block,
        u32 home) {
    if (!query_block || home >= block_entry_width_facts.size()) {
        return {};
    }
    BuildFunctionWidthFacts();
    const auto found = function_entry_width_facts.find(query_block);
    return found != function_entry_width_facts.end()
            ? found->second[home]
            : ExtensionFacts{};
}

}  // namespace swift::runtime::backend::arm64
