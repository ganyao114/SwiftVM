#include "function.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::x64 {
bool NeedsFunctionFrame(std::span<ir::HIRBlock* const> blocks) {
    for (auto* block : blocks) {
        std::unordered_set<ir::Inst*> local;
        for (auto& inst : block->GetInstList()) local.insert(&inst);
        for (auto& inst : block->GetInstList()) {
            if (inst.GetOp() == ir::OpCode::AddPhi) return true;
            for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
                auto& arg = inst.ArgAt(slot);
                if (arg.IsValue() && !local.contains(arg.Get<ir::Value>().Def())) return true;
                if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue() && !local.contains(arg.Get<ir::Lambda>().GetValue().Def())) return true;
                if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>())
                    if (param.data.IsValue() && !local.contains(param.data.value.Def())) return true;
            }
        }
        bool foreign{};
        const auto terminal = [&](const auto& recurse, const ir::Terminal& term) -> void {
            VisitVariant<void>(term, [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ir::terminal::If>) {
                    foreign |= !local.contains(value.cond.Def()); recurse(recurse, value.then_); recurse(recurse, value.else_);
                } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                    recurse(recurse, value.then_); recurse(recurse, value.else_);
                } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                    foreign |= !local.contains(value.value.Def()); for (const auto& item : value.cases) recurse(recurse, item.then);
                } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, value.else_);
            });
        };
        terminal(terminal, block->GetBlock()->GetTerminal());
        if (foreign) return true;
    }
    return false;
}
} // namespace swift::runtime::backend::x64
