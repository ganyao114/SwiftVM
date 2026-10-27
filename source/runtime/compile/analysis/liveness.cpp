#include "liveness.h"

#include <algorithm>

#include "facts.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {

std::span<const HostRegisterWrites::Write> HostRegisterWrites::For(u16 reg) const {
    const auto first =
            std::lower_bound(writes.begin(), writes.end(), reg, [](const Write& write, u16 key) {
                return write.reg < key;
            });
    const auto last = std::upper_bound(
            first, writes.end(), reg, [](u16 key, const Write& write) { return key < write.reg; });
    return {first, last};
}

bool HostRegisterWrites::Crosses(u16 reg, u32 after, u32 through) const {
    for (const auto& write : For(reg))
        if (after < write.position && write.position <= through) return true;
    return false;
}

u32 ValueLiveness::UseEnd(u32 id) const { return id < use_ends.size() ? use_ends[id] : 0; }

u32 ValueLiveness::FunctionEnd(u32 id) const {
    const auto found = std::lower_bound(
            recorded_ends.begin(), recorded_ends.end(), id, [](const auto& item, u32 key) {
                return item.first < key;
            });
    return found != recorded_ends.end() && found->first == id ? found->second
                                                              : std::max(id, UseEnd(id));
}

u32 ValueLiveness::DefinitionBlockEnd(const ir::Inst* definition) const {
    ASSERT(function);
    const auto* value = function->GetHIRValue(ir::Value{definition});
    if (!value) return 0;
    const auto& list = value->GetBlock()->GetBlock()->GetInstList();
    return list.empty() ? 0 : list.back().Id();
}

bool ValueLiveness::UsesStayInBlock(const ir::Block* block, ir::Value value) const {
    if (!function) return true;
    value = ir::ResolveBitCastSource(value);
    if (!value.Defined() || value.Id() >= block_local.size() || !block_local[value.Id()])
        return false;
    const auto* definition = function->GetHIRValue(value);
    return definition && definition->GetBlock()->GetBlock() == block;
}

}  // namespace swift::runtime::compile
