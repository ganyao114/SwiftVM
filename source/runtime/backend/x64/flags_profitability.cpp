#include "flags_profitability.h"
#include "translator.h"
#include "flags_usage.h"
#include "runtime/common/variant_util.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;

struct Effects {
    u32 reads{}, writes{}, carry_reads{}, helper_calls{};
    bool supported{true};
};

std::optional<u32> ConditionReads(ir::Cond condition) {
    switch (condition) {
        case ir::Cond::AL: case ir::Cond::NV: return 0;
        case ir::Cond::GT: case ir::Cond::LE: return 2;
        case ir::Cond::EQ: case ir::Cond::NE: case ir::Cond::CS: case ir::Cond::CC:
        case ir::Cond::MI: case ir::Cond::PL: case ir::Cond::VS: case ir::Cond::VC:
        case ir::Cond::HI: case ir::Cond::LS: case ir::Cond::GE: case ir::Cond::LT:
            return 1;
        default: return std::nullopt;
    }
}

Effects Classify(const ir::Inst& inst) {
    // Positive inventory: adding or changing a lowering requires an explicit
    // profitability audit. A forwarding barrier is not a helper-call census.
    switch (inst.GetOp()) {
        case O::Adc: case O::Sbb: return {.carry_reads = 1};
        case O::GetFlags: return {.reads = 1};
        case O::SaveFlags: case O::BranchOnlyFlags: {
            const auto* producer = inst.GetArg<ir::Value>(0).Def();
            if (!producer) return {.supported = false};
            return ClassifyProducerFlags(producer->GetOp(), producer->ReturnType(),
                inst.GetArg<ir::Flags>(1)).published_mask ? Effects{.reads = 1, .writes = 1} : Effects{};
        }
        case O::PublishFCmpFlags: return {.reads = 1, .writes = 1};
        case O::PublishSse42StrFlags:
            return True(inst.GetArg<ir::Flags>(1) & ir::Flags::All)
                ? Effects{.reads = 1, .writes = 1} : Effects{};
        case O::ClearFlags:
            return True(inst.GetArg<ir::Flags>(0) & ir::Flags::All)
                ? Effects{.reads = 1, .writes = 1} : Effects{};
        case O::TestFlags: case O::TestNotFlags:
            return True(inst.GetArg<ir::Flags>(0) & ir::Flags::All) ? Effects{.reads = 1} : Effects{};
        case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
            return {.reads = 1, .writes = 1};
        case O::CondSet: case O::LocalCondSet: case O::CondSelect: {
            const auto reads = ConditionReads(inst.GetArg<ir::Cond>(0));
            return reads ? Effects{.reads = *reads} : Effects{.supported = false};
        }
        // Calls always flush then reload the resident word. Conditional
        // Div128's slow call is charged even when its native fast path wins.
        case O::CallLambda: case O::CallDynamic: case O::CallLocation:
        case O::Cpuid: case O::Div: case O::SignedDiv64: case O::Div128:
        case O::PopCount: case O::FCmpCondSet: case O::X87Op:
            return {.helper_calls = 1};
        // Scalar memory address admission calls CheckAndBeginMemory once;
        // EndMemoryAccess releases the lease inline. Atomic lowerings add an
        // EndMemory call, whereas CAS128 has CheckMemory + atomic fallback.
        case O::LoadMemory: case O::LoadMemoryTSO: case O::StoreMemory: case O::StoreMemoryTSO:
            return {.helper_calls = 1};
        case O::CompareAndSwap: case O::AtomicExchange: case O::AtomicFetchAdd: case O::AtomicRMW:
        case O::CompareAndSwap128:
            return {.helper_calls = 2};
        case O::MemoryCopy: case O::MemoryCopyTSO:
            return inst.GetArg<ir::Imm>(2).Get() ? Effects{.helper_calls = 3} : Effects{};
        // Known inline scalar/control/container lowerings. Their native
        // flags are temporaries and do not access the packed guest word.
        case O::DefineLocal: case O::LoadLocal: case O::StoreLocal:
        case O::LoadUniform: case O::StoreUniform: case O::GetUniformAddress:
        case O::GetHostGPR: case O::GetHostFPR: case O::SetHostGPR: case O::SetHostFPR:
        case O::MemoryBarrierTSO: case O::CheckMemoryAlignment:
        case O::UniformBarrier: case O::XchgBarrier: case O::BranchOnlyEdges:
        case O::PushRSB: case O::PopRSB: case O::CallReturn:
        case O::Nop: case O::AdvancePC: case O::SetLocation: case O::GetLocation:
        case O::Zero: case O::LoadImm: case O::BitCast: case O::GetOperand: case O::GetResult:
        case O::Select: case O::SelectZero: case O::Goto: case O::NotGoto:
        case O::BindLabel: case O::AddPhi:
        case O::Add: case O::Sub: case O::Neg: case O::Mul: case O::MulSub: case O::MulHigh:
        case O::Div128Remainder: case O::CpuidUpper:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::And: case O::Or: case O::Xor: case O::Not: case O::AndNot:
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue:
        case O::ByteSwap: case O::CountLeadingZeros64: case O::CountLeadingZeros32:
        case O::CountTrailingZeros64: case O::CountTrailingZeros32:
        case O::BitExtract: case O::BitInsert: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
        case O::Crc32: case O::Crc32c: case O::LocalParitySet:
            return {};
        default: return {.supported = false};
    }
}

struct Census {
    FlagsResidencyDecision decision;
    bool unsupported{}, saturated{};

    void Add(u64& sum, u64 value, u32 weight = 1) {
        const auto max = std::numeric_limits<u64>::max();
        if (weight && value > max / weight) { saturated = true; return; }
        const auto weighted = value * weight;
        if (sum > max - weighted) { saturated = true; return; }
        sum += weighted;
    }

    void MergeBranches(const Census& then_census, const Census& else_census) {
        // Savings must occur on either branch; mutually exclusive predicates
        // cannot be credited together. Helper overhead uses the worse branch.
        const auto& then_decision = then_census.decision;
        const auto& else_decision = else_census.decision;
        Add(decision.weighted_flag_reads,
            std::min(then_decision.weighted_flag_reads, else_decision.weighted_flag_reads));
        Add(decision.weighted_flag_writes,
            std::min(then_decision.weighted_flag_writes, else_decision.weighted_flag_writes));
        Add(decision.weighted_carry_reads,
            std::min(then_decision.weighted_carry_reads, else_decision.weighted_carry_reads));
        Add(decision.weighted_helper_calls,
            std::max(then_decision.weighted_helper_calls, else_decision.weighted_helper_calls));
        Add(decision.weighted_exit_boundaries, then_decision.weighted_exit_boundaries);
        Add(decision.weighted_exit_boundaries, else_decision.weighted_exit_boundaries);
        unsupported |= then_census.unsupported || else_census.unsupported;
        saturated |= then_census.saturated || else_census.saturated;
    }

    void Terminal(const ir::Terminal& terminal, u32 weight, unsigned depth = 0) {
        // Same terminal-depth bound as CfgRegisterPlan::TerminalInputs.
        if (depth > 128) { unsupported = true; return; }
        VisitVariant<void>(terminal, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                const auto reads = ConditionReads(term.cond);
                if (!reads) { unsupported = true; return; }
                Add(decision.weighted_flag_reads, *reads, weight);
                Census then_census, else_census;
                then_census.Terminal(term.then_, weight, depth + 1);
                else_census.Terminal(term.else_, weight, depth + 1);
                MergeBranches(then_census, else_census);
            } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
                Census then_census, else_census;
                then_census.Terminal(term.then_, weight, depth + 1);
                else_census.Terminal(term.else_, weight, depth + 1);
                MergeBranches(then_census, else_census);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                Add(decision.weighted_exit_boundaries, 1, weight);
                Census continuation;
                continuation.Terminal(term.else_, weight, depth + 1);
                // The halted path reaches the shared exit without evaluating
                // the continuation's predicates: guaranteed flags savings
                // are zero, while the continuation is the worst helper path.
                Add(decision.weighted_helper_calls, continuation.decision.weighted_helper_calls);
                Add(decision.weighted_exit_boundaries, continuation.decision.weighted_exit_boundaries);
                unsupported |= continuation.unsupported;
                saturated |= continuation.saturated;
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                // The no-match path has zero flags accesses. Preserve all
                // potential exits for diagnosis and charge the worst helper
                // branch, but credit no conditional case-body savings.
                u64 helper_calls{};
                for (const auto& item : term.cases) {
                    Census case_census;
                    case_census.Terminal(item.then, weight, depth + 1);
                    helper_calls = std::max(helper_calls, case_census.decision.weighted_helper_calls);
                    Add(decision.weighted_exit_boundaries, case_census.decision.weighted_exit_boundaries);
                    unsupported |= case_census.unsupported;
                    saturated |= case_census.saturated;
                }
                Add(decision.weighted_helper_calls, helper_calls);
                Add(decision.weighted_exit_boundaries, 1, weight); // No matched case.
            } else if constexpr (std::is_same_v<T, ir::terminal::Invalid> ||
                std::is_same_v<T, ir::terminal::ReturnToDispatch> ||
                std::is_same_v<T, ir::terminal::ReturnToHost> ||
                std::is_same_v<T, ir::terminal::LinkBlock> ||
                std::is_same_v<T, ir::terminal::LinkBlockFast> ||
                std::is_same_v<T, ir::terminal::ExternalLinkBlock> ||
                std::is_same_v<T, ir::terminal::PopRSBHint>) {
                Add(decision.weighted_exit_boundaries, 1, weight);
            } else unsupported = true;
        });
    }
};
FlagsResidencyDecision Select(ir::Block* body,
    const backend::FunctionPlan& function, CfgRegisterOptions options,
    std::optional<CfgRegisterPlan>* retained, bool reuse_interference) {
    if (retained) retained->reset();
    Census census;
    options.physical_registers = {3, 5, 15};
    auto off = CfgRegisterPlan::Build(body, function, options);
    ++census.decision.allocation_builds;
    if (!off.Enabled() || !off.Validate()) {
        census.decision.reason = FlagsResidencyReason::InvalidPlan; return census.decision;
    }
    options.physical_registers = {5, 15};
    // Recolor owns a deep copy; no allocation token or mutable analysis is
    // shared across budgets. Dense construction words have already expired.
    auto on = reuse_interference ? off.Recolor(options.physical_registers)
                                 : CfgRegisterPlan::Build(body, function, options);
    if (reuse_interference) ++census.decision.allocation_recolors;
    else ++census.decision.allocation_builds;
    if (!on.Enabled() || !on.Validate()) {
        census.decision.reason = FlagsResidencyReason::InvalidPlan; return census.decision;
    }
    const auto& instructions = on.Instructions();
    const auto& blocks = on.Blocks();
    for (const auto& block : blocks) {
        if (!block.reachable) continue;
        const auto weight = block.loop_weight;
        for (u32 index = block.begin; index < block.end; ++index) {
            auto* inst = instructions.at(index);
            // Match Translate's replacement-marker precedence exactly.
            if (const auto exit = function.exits.find(inst); exit != function.exits.end()) {
                census.Terminal(exit->second, weight); continue;
            }
            if (function.halt_checks.contains(inst)) {
                census.Add(census.decision.weighted_exit_boundaries, 1, weight); continue;
            }
            const auto effect = Classify(*inst);
            census.unsupported |= !effect.supported;
            census.Add(census.decision.weighted_flag_reads, effect.reads, weight);
            census.Add(census.decision.weighted_flag_writes, effect.writes, weight);
            census.Add(census.decision.weighted_carry_reads, effect.carry_reads, weight);
            census.Add(census.decision.weighted_helper_calls, effect.helper_calls, weight);
        }
    }
    // A final stop suppresses the body terminal dynamically. The final
    // lexical block can itself be unreachable after an earlier terminal.
    if (instructions.empty()) census.Terminal(body->GetTerminal(), 1);
    else if (!blocks.empty() && blocks.back().reachable && !function.stops.contains(instructions.back()))
        census.Terminal(body->GetTerminal(), blocks.back().loop_weight);

    u64 off_benefit{}, on_benefit{};
    const auto benefit = [&](const CfgRegisterPlan& plan, u64& sum) {
        for (const auto& allocation : plan.Allocations()) {
            // Public planner counters saturate independently. Never make a
            // policy decision from an underreported register opportunity.
            if (allocation.weighted_benefit == std::numeric_limits<u32>::max())
                census.saturated = true;
            if (allocation.slot) census.Add(sum, allocation.weighted_benefit);
        }
    };
    benefit(off, off_benefit); benefit(on, on_benefit);
    census.decision.scalar_lost_benefit = off_benefit > on_benefit ? off_benefit - on_benefit : 0;
    // Entry reload and one shared epilogue flush. Every admitted body helper
    // adds a flush/reload pair; its own internals are identical in both modes.
    census.decision.resident_overhead = 2;
    census.Add(census.decision.resident_overhead, census.decision.weighted_helper_calls, 2);
    u64 savings{};
    census.Add(savings, census.decision.weighted_flag_reads);
    census.Add(savings, census.decision.weighted_flag_writes);
    census.Add(savings, census.decision.weighted_carry_reads);
    u64 cost = census.decision.resident_overhead;
    census.Add(cost, census.decision.scalar_lost_benefit);
    census.decision.net_savings = savings > cost ? savings - cost : 0;
    if (census.saturated) census.decision.reason = FlagsResidencyReason::SaturatedEstimate;
    else if (census.unsupported) census.decision.reason = FlagsResidencyReason::Unsupported;
    else if (!savings) census.decision.reason = FlagsResidencyReason::NoFlags;
    else if (savings <= cost) census.decision.reason = FlagsResidencyReason::Unprofitable;
    else {
        census.decision.selected = true;
        census.decision.reason = FlagsResidencyReason::Profitable;
    }
    if (retained) *retained = census.decision.selected ? std::move(on) : std::move(off);
    return census.decision;
}
} // namespace

FlagsResidencyDecision SelectFlagsResidency(ir::Block* body,
    const backend::FunctionPlan& function, CfgRegisterOptions options, bool reuse_interference) {
    return Select(body, function, std::move(options), nullptr, reuse_interference);
}

FlagsResidencyDecision Translator::SelectFlagsForPreparation(ir::Block* body,
    const backend::FunctionPlan& function, CfgRegisterOptions options,
    std::optional<CfgRegisterPlan>& allocation, bool retain_final, bool reuse_interference) {
    // The only plan-retaining entry is private synchronous preparation. It
    // must be called after promotion and before forwarding; retention is
    // permitted only when there can be no later forwarding mutation.
    allocation.reset();
    return Select(body, function, std::move(options), retain_final ? &allocation : nullptr, reuse_interference);
}

const char* FlagsResidencyReasonName(FlagsResidencyReason reason) noexcept {
    switch (reason) {
        case FlagsResidencyReason::Disabled: return "disabled";
        case FlagsResidencyReason::Forced: return "forced";
        case FlagsResidencyReason::NoFlags: return "no_flags";
        case FlagsResidencyReason::Unsupported: return "unsupported";
        case FlagsResidencyReason::InvalidPlan: return "invalid_plan";
        case FlagsResidencyReason::SaturatedEstimate: return "saturated_estimate";
        case FlagsResidencyReason::Unprofitable: return "unprofitable";
        case FlagsResidencyReason::Profitable: return "profitable";
    }
    return "unknown";
}
} // namespace swift::runtime::backend::x64
