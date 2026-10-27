#include <algorithm>
#include <array>
#include "edge_flags.h"
#include "runtime/backend/arm64/compile/register_contract.h"

namespace swift::runtime::backend::arm64 {
namespace {
constexpr auto SubsetMasks() {
    std::array<u16, 16> result{};
    for (unsigned overwritten = 0; overwritten < 16; ++overwritten)
        for (unsigned mask = 0; mask < 16; ++mask)
            if ((mask & ~overwritten) == 0) result[overwritten] |= u16{1} << mask;
    return result;
}
constexpr auto subset_masks = SubsetMasks();
u32 HostMask(ir::Flags flags) {
    u32 mask = 0;
    if (True(flags & ir::Flags::Negate)) mask |= 1u << 31;
    if (True(flags & ir::Flags::Zero)) mask |= 1u << 30;
    if (True(flags & ir::Flags::Carry)) mask |= 1u << 29;
    if (True(flags & ir::Flags::Overflow)) mask |= 1u << 28;
    return mask;
}
}  // namespace
EdgeFlagsTargetContract AnalyzeBlockEdgeFlags(const ir::Block& block,
                                              const std::function<bool(const ir::Inst&)>& barrier) {
    ir::Flags remaining = ir::Flags::NZCV;
    EdgeFlagsTargetContract contract{};
    auto overwrite = [&](ir::Flags flags_mask) {
        remaining &= ~flags_mask;
        contract.overwrite_before_observe =
                static_cast<u32>(HostMask(ir::Flags::NZCV & ~remaining));
    };
    auto observes = [&](ir::Flags flags_mask) {
        const auto observed = static_cast<u32>(HostMask(remaining & flags_mask & ir::Flags::NZCV));
        contract.observed_nzcv_mask |= static_cast<u8>(observed >> 28);
    };

    for (const auto& inst : block.GetInstList()) {
        const auto op = inst.GetOp();
        if (barrier(inst)) {
            contract.barrier_before_commit = true;
            return contract;
        }
        switch (op) {
            case ir::OpCode::TestFlags:
            case ir::OpCode::TestNotFlags:
                observes(inst.GetArg<ir::Flags>(0));
                break;
            case ir::OpCode::GetFlags:
                observes(ir::Flags::NZCV);
                break;
            case ir::OpCode::Adc:
            case ir::OpCode::Sbb:
                observes(ir::Flags::Carry);
                break;
            case ir::OpCode::InvertCarry:
                observes(ir::Flags::Carry);
                break;
            case ir::OpCode::CondSelect:
            case ir::OpCode::CondSet:
                observes(ir::Flags::NZCV);
                break;
            case ir::OpCode::LocalCondSet:
                observes(ir::Flags::NZCV);
                break;
            case ir::OpCode::Goto:
            case ir::OpCode::NotGoto:
            case ir::OpCode::BindLabel:
                contract.barrier_before_commit = true;
                return contract;
            case ir::OpCode::SaveFlags:
                overwrite(inst.GetArg<ir::Flags>(1));
                break;
            case ir::OpCode::ClearFlags:
                overwrite(inst.GetArg<ir::Flags>(0));
                break;
            case ir::OpCode::SetCarry:
                overwrite(ir::Flags::Carry);
                break;
            case ir::OpCode::SetOverflow:
                overwrite(ir::Flags::Overflow);
                break;
            case ir::OpCode::PublishFCmpFlags:
                overwrite(ir::Flags::NZCV);
                break;
            case ir::OpCode::BranchOnlyFlags:
                overwrite(ir::Flags::NZCV);
                break;
            case ir::OpCode::PublishSse42StrFlags:
                overwrite(inst.GetArg<ir::Flags>(1));
                break;
            case ir::OpCode::AdvancePC:
                contract.commits_before_fault =
                        contract.overwrite_before_observe != 0 && !contract.barrier_before_commit;
                return contract;
            default:
                break;
        }
    }
    return contract;
}

namespace {
template <bool SummarizeWrites>
bool AnalyzeTransparency(const ir::Block& block, bool* writes_flags) {
    bool transparent = true;
    if constexpr (SummarizeWrites) *writes_flags = false;
    for (auto& inst : block.GetInstList()) {
        const auto effects = inst.GetEffects();
        if constexpr (SummarizeWrites) {
            if (!*writes_flags)
                *writes_flags = True(effects &
                                     (ir::IREffect::WriteFlags | ir::IREffect::Unknown));
        }
        if (True(effects & ir::IREffect::MayFault)) {
            transparent = false;
            if constexpr (!SummarizeWrites) return false;
        }
        switch (inst.GetOp()) {
            case ir::OpCode::GetFlags:
            case ir::OpCode::CallLambda:
            case ir::OpCode::CallLocation:
            case ir::OpCode::CallDynamic:
            case ir::OpCode::X87Op:
            case ir::OpCode::TestFlags:
            case ir::OpCode::TestNotFlags:
            case ir::OpCode::SaveFlags:
            case ir::OpCode::BranchOnlyFlags:
            case ir::OpCode::SetCarry:
            case ir::OpCode::SetOverflow:
            case ir::OpCode::ClearFlags:
            case ir::OpCode::InvertCarry:
            case ir::OpCode::Adc:
            case ir::OpCode::Sbb:
            case ir::OpCode::CondSelect:
            case ir::OpCode::CondSet:
            // Raw predicates can leave host NZCV distinct from guest flags.
            case ir::OpCode::TestZero:
            case ir::OpCode::TestNotZero:
            case ir::OpCode::VecFCmp:
            case ir::OpCode::FCmpCondSet:
            case ir::OpCode::PublishFCmpFlags:
                transparent = false;
                if constexpr (!SummarizeWrites) return false;
                break;
            default:
                break;
        }
        // An early observer settles transparency but not whether a later
        // producer can leave pending NZCV at the terminal.
        if constexpr (SummarizeWrites)
            if (!transparent && *writes_flags) return false;
    }
    return transparent;
}
}  // namespace

bool AnalyzeBlockFlagsTransparent(const ir::Block& block, bool* writes_flags) {
    return writes_flags ? AnalyzeTransparency<true>(block, writes_flags)
                        : AnalyzeTransparency<false>(block, nullptr);
}

bool AnalyzeBlockTokenOverwrite(const ir::Block& block,
                                const std::function<bool(const ir::Inst&)>& barrier) {
    ir::Flags needed = ir::Flags::Parity | ir::Flags::AuxiliaryCarry;
    for (auto& inst : block.GetInstList()) {
        if (barrier(inst)) {
            return false;
        }
        if (inst.GetOp() == ir::OpCode::GetFlags || inst.GetOp() == ir::OpCode::TestFlags ||
            inst.GetOp() == ir::OpCode::TestNotFlags) {
            return false;
        }
        if (inst.GetOp() == ir::OpCode::SaveFlags) {
            needed &= ~inst.GetArg<ir::Flags>(1);
        } else if (inst.GetOp() == ir::OpCode::ClearFlags) {
            needed &= ~inst.GetArg<ir::Flags>(0);
        }
        if (!True(needed)) {
            return true;
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            return false;
        }
    }
    return false;
}

u16 AnalyzeBlockEdgeAcceptance(const ir::Block& block) {
    u16 accepted = 0;
    u32 overwritten = 0;
    for (const auto& inst : block.GetInstList()) {
        // A fault can observe every incoming bit not overwritten yet.
        if (True(inst.GetEffects() & ir::IREffect::MayFault)) return accepted;
        switch (inst.GetOp()) {
            case ir::OpCode::GetFlags:
            case ir::OpCode::CallLambda:
            case ir::OpCode::CallLocation:
            case ir::OpCode::CallDynamic:
            case ir::OpCode::X87Op:
            case ir::OpCode::TestFlags:
            case ir::OpCode::TestNotFlags:
            case ir::OpCode::Adc:
            case ir::OpCode::Sbb:
            case ir::OpCode::CondSelect:
            case ir::OpCode::CondSet:
                return accepted;
            case ir::OpCode::ClearFlags:
                overwritten |= HostMask(inst.GetArg<ir::Flags>(0)) >> 28;
                break;
            case ir::OpCode::SaveFlags:
                overwritten |= HostMask(inst.GetArg<ir::Flags>(1)) >> 28;
                break;
            case ir::OpCode::BranchOnlyFlags:
                return UINT16_MAX;
            default:
                break;
        }
        // Includes mask zero only after reaching a non-observing instruction,
        // matching the old query's ordering even for empty/observer-only blocks.
        accepted |= subset_masks[overwritten];
        if (accepted == UINT16_MAX) break;
    }
    return accepted;
}

bool HasConditionalEdgeTerminal(const ir::Terminal& terminal) {
    const auto conditional = [&](auto&& self, const ir::Terminal& terminal) -> bool {
        return VisitVariant<bool>(terminal, [&](const auto& edge) {
            using T = std::decay_t<decltype(edge)>;
            if constexpr (std::is_same_v<T, ir::terminal::If> ||
                          std::is_same_v<T, ir::terminal::Condition>)
                return true;
            else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>)
                return self(self, edge.else_);
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                for (const auto& arm : edge.cases)
                    if (self(self, arm.then)) return true;
            }
            return false;
        });
    };
    return conditional(conditional, terminal);
}

bool MayQueryEdgeAcceptance(const ir::Block& block) {
    // Each block resets the emitter's pending NZCV state. Only a conditional
    // terminal after a flag write can ask a successor to accept pending bits.
    if (!HasConditionalEdgeTerminal(block.GetTerminal())) return false;
    bool writes_flags;
    AnalyzeBlockFlagsTransparent(block, &writes_flags);
    return writes_flags;
}

std::vector<u16> ResolveEdgeAcceptance(std::span<const EdgeAcceptanceNode> nodes) {
    std::vector<u16> result(nodes.size());
    std::vector<u8> state(nodes.size());
    std::vector<u32> path;
    path.reserve(nodes.size());
    for (u32 start = 0; start < nodes.size(); ++start) {
        if (state[start] == 2) continue;
        path.clear();
        u32 next = start;
        while (next < nodes.size() && state[next] == 0) {
            state[next] = 1;
            path.push_back(next);
            next = nodes[next].successor;
        }
        u16 accepted = next < nodes.size() && state[next] == 2 ? result[next] : 0;
        if (next < nodes.size() && state[next] == 1) {
            // Every entry into a cycle sees its complete union before revisiting
            // a block. Masks absent from that union still require a commit.
            auto cycle = std::find(path.begin(), path.end(), next);
            for (auto it = cycle; it != path.end(); ++it) accepted |= nodes[*it].accepted;
            for (auto it = cycle; it != path.end(); ++it) {
                result[*it] = accepted;
                state[*it] = 2;
            }
            path.erase(cycle, path.end());
        }
        while (!path.empty()) {
            const auto node = path.back();
            path.pop_back();
            accepted |= nodes[node].accepted;
            result[node] = accepted;
            state[node] = 2;
        }
    }
    return result;
}

}  // namespace swift::runtime::backend::arm64
