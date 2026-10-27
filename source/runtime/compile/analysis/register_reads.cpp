#include "register_reads.h"

#include <algorithm>

#include "facts.h"
#include "runtime/ir/hir_builder.h"
#include "uses.h"

namespace swift::runtime::compile {

RegisterReadUses::RegisterReadUses(const ir::Block* block, const ir::HIRFunction* function) {
    ASSERT((block != nullptr) != (function != nullptr));
    const auto visit_blocks = [&](auto&& visit) {
        if (function) {
            for (const auto& member : function->GetHIRBlocksRPO()) visit(*member.GetBlock());
        } else {
            visit(*block);
        }
    };
    // Store only the declaration family consumed by capture preparation,
    // rather than allocating a hash node for every operand definition.
    visit_blocks([&](const ir::Block& current) {
        for (const auto& inst : current.GetInstList())
            if (inst.GetOp() == ir::OpCode::GetHostGPR) reads.push_back({inst.Id()});
    });
    std::sort(reads.begin(), reads.end(), [](const Entry& a, const Entry& b) {
        return a.value < b.value;
    });
    reads.erase(std::unique(reads.begin(),
                            reads.end(),
                            [](const Entry& a, const Entry& b) { return a.value == b.value; }),
                reads.end());
    if (reads.empty()) return;
    const auto add = [&](ir::Value value, u32 point) {
        if (!value.Defined()) return;
        const auto width =
                value.Type() == ir::ValueType::VOID ? 0 : ir::GetValueSizeByte(value.Type());
        value = ir::ResolveBitCastSource(value);
        if (!value.Defined()) return;
        const auto found = std::lower_bound(
                reads.begin(), reads.end(), value.Id(), [](const Entry& entry, u32 id) {
                    return entry.value < id;
                });
        if (found == reads.end() || found->value != value.Id()) return;
        ASSERT(point <= UINT16_MAX && width <= UINT8_MAX);
        found->demand.last_use = std::max<u32>(found->demand.last_use, point);
        found->demand.max_width = std::max<u32>(found->demand.max_width, width);
        found->seen = true;
    };
    visit_blocks([&](const ir::Block& current) {
        // Virtual aliases and pseudo operands still have semantic widths.
        // This is deliberately distinct from physical ValueLiveness::UseEnd.
        for (const auto& inst : current.GetInstList())
            inst.ForEachValue([&](ir::Value value) { add(value, inst.Id()); });
        const auto& list = current.GetInstList();
        VisitImplicitUses(current, list.empty() ? 0 : list.back().Id(), add);
    });
}

std::optional<RegisterReadUses::Demand> RegisterReadUses::Find(u32 value) const {
    const auto found =
            std::lower_bound(reads.begin(), reads.end(), value, [](const Entry& entry, u32 id) {
                return entry.value < id;
            });
    if (found == reads.end() || found->value != value || !found->seen) return std::nullopt;
    return found->demand;
}

}  // namespace swift::runtime::compile
