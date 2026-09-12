#include "base/logging.h"
//
// Created by 甘尧 on 2024/6/26.
//
// Backward flag liveness, in the spirit of FEX's
// RedundantFlagCalculationElimination: walk the block from the end, track the
// set of guest flag bits that are still needed, and drop flag writes whose
// bits are all rewritten later before any read.
//
// Flag model in this IR (see ir.inc):
//   writes: SaveFlags(value, mask) / ClearFlags(mask) / SetCarry / SetOverflow
//   reads:  TestFlags / TestNotFlags (mask), GetFlags (whole register),
//           Adc / Sbb (implicit C), CondSelect / CondSet (implicit NZCV via
//           host cond)
// The guest flags live in the backend flags register (JIT) / state word
// (interpreter) across blocks, so every bit is live-out of a block.
//
// Removal is safe in both backends: a removed SaveFlags only makes its def
// emit the non-flag-setting instruction form, so no later reader can observe
// it (readers make their bits needed, which keeps the last write). ClearFlags
// remains an independent write even when a sibling SaveFlags is removed.

#include "flags_elimination_pass.h"
#include "flags_carry_regions.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fmt/format.h"
#include "runtime/common/variant_util.h"


// 诊断开关来自进程级配置快照，不在逐块路径重复扫描 environ。
static bool EnvDumpIr() { return swift::runtime::GetSvmConfig().dump_ir; }
static bool EnvDumpIrPost() { return swift::runtime::GetSvmConfig().dump_ir_post; }
static bool EnvFlagsDebug() { return swift::runtime::GetSvmConfig().flags_debug; }

namespace swift::runtime::ir {

namespace {

bool BranchOnlyEnabled(const FeatureSet& features) {
    // Default ON after the flip A/B; =0 is the full-materialization rollback.
    return features.flags_branch_only;
}

bool IsHelperBoundary(OpCode op) {
    return op == OpCode::CallLambda || op == OpCode::CallLocation ||
           op == OpCode::CallDynamic || op == OpCode::X87Op;
}

std::optional<Flags> ConditionFlags(Cond cond) {
    switch (cond) {
        case Cond::EQ:
        case Cond::NE:
            return Flags::Zero;
        case Cond::MI:
        case Cond::PL:
            return Flags::Negate;
        case Cond::VS:
        case Cond::VC:
            return Flags::Overflow;
        case Cond::CS:
        case Cond::CC:
            return Flags::Carry;
        case Cond::HI:
        case Cond::LS:
            return Flags::Carry | Flags::Zero;
        case Cond::GE:
        case Cond::LT:
            return Flags::Negate | Flags::Overflow;
        case Cond::GT:
        case Cond::LE:
            return Flags::Negate | Flags::Overflow | Flags::Zero;
        default:
            return std::nullopt;
    }
}

Flags TransferFlagsLiveness(Block* block, Flags needed) {
    for (auto it = block->GetInstList().rbegin();
         it != block->GetInstList().rend(); ++it) {
        auto& inst = *it;
        switch (inst.GetOp()) {
            case OpCode::SaveFlags:
            case OpCode::BranchOnlyFlags:
                needed &= ~inst.GetArg<Flags>(1);
                break;
            case OpCode::ClearFlags:
                needed &= ~inst.GetArg<Flags>(0);
                break;
            case OpCode::SetCarry:
                needed &= ~Flags::Carry;
                break;
            case OpCode::SetOverflow:
                needed &= ~Flags::Overflow;
                break;
            case OpCode::PublishFCmpFlags:
                needed &= ~Flags::All;
                break;
            case OpCode::PublishSse42StrFlags:
                needed &= ~inst.GetArg<Flags>(1);
                break;
            case OpCode::TestFlags:
            case OpCode::TestNotFlags:
                needed |= inst.GetArg<Flags>(0);
                break;
            case OpCode::GetFlags:
                needed |= Flags::All;
                break;
            case OpCode::Adc:
            case OpCode::Sbb:
                needed |= Flags::Carry;
                break;
            case OpCode::CondSelect:
            case OpCode::CondSet:
            case OpCode::LocalCondSet:
                needed |= ConditionFlags(inst.GetArg<Cond>(0))
                                  .value_or(Flags::NZCV);
                break;
            case OpCode::LocalParitySet:
            case OpCode::FCmpCondSet:
                break;
            default:
                if (IsHelperBoundary(inst.GetOp())) {
                    needed |= Flags::All;
                }
                break;
        }
    }
    return needed;
}

void CollectTerminalTargets(const Terminal& terminal,
                            std::vector<Location>& targets,
                            bool& unknown) {
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, terminal::LinkBlock> ||
                      std::is_same_v<T, terminal::LinkBlockFast>) {
            targets.push_back(term.next);
        } else if constexpr (std::is_same_v<T, terminal::If>) {
            CollectTerminalTargets(term.then_, targets, unknown);
            CollectTerminalTargets(term.else_, targets, unknown);
        } else if constexpr (std::is_same_v<T, terminal::Switch>) {
            for (const auto& case_ : term.cases) {
                CollectTerminalTargets(case_.then, targets, unknown);
            }
        } else {
            // Return/dispatcher/helper/check-halt/Condition edges may leave
            // this decoded CFG. Their successor flag demand is unknowable.
            unknown = true;
        }
    });
}

using LiveMap = std::unordered_map<HIRBlock*, Flags>;

Flags ComputeBlockLiveOut(HIRBlock* hir_block, const LiveMap& live_in) {
    auto* block = hir_block->GetBlock();
    if (block->GetInstList().empty() || !block->HasTerminal()) {
        return Flags::All;
    }

    std::vector<Location> targets;
    bool unknown = false;
    CollectTerminalTargets(block->GetTerminal(), targets, unknown);
    Flags live_out = unknown ? Flags::All : Flags::None;
    for (const auto& target : targets) {
        HIRBlock* successor = nullptr;
        for (auto* candidate : hir_block->GetSuccessors()) {
            if (candidate &&
                candidate->GetBlock()->GetStartLocation() == target) {
                successor = candidate;
                break;
            }
        }
        const auto live = successor ? live_in.find(successor) : live_in.end();
        if (!successor || live == live_in.end() ||
            successor->GetBlock()->GetInstList().empty() ||
            !successor->GetBlock()->HasTerminal()) {
            live_out |= Flags::All;
        } else {
            live_out |= live->second;
        }
    }
    return live_out;
}

LiveMap ComputeFunctionLiveIn(HIRFunction* function) {
    LiveMap live_in;
    for (auto* hir_block : function->GetHIRBlocks()) {
        if (hir_block) {
            live_in[hir_block] = Flags::None;
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        auto& rpo = function->GetHIRBlocksRPO();
        for (auto it = rpo.rbegin(); it != rpo.rend(); ++it) {
            auto* hir_block = &*it;
            auto* block = hir_block->GetBlock();
            const Flags next = TransferFlagsLiveness(
                    block, ComputeBlockLiveOut(hir_block, live_in));
            if (next != live_in[hir_block]) {
                live_in[hir_block] = next;
                changed = true;
            }
        }
    }
    return live_in;
}

bool IsBranchOnlyProducer(OpCode op) {
    return op == OpCode::Add || op == OpCode::Sub ||
           op == OpCode::And || op == OpCode::Or ||
           op == OpCode::Xor || op == OpCode::AndNot;
}

bool IsPolarityStoreShape(const Inst* inst) {
    if (inst->GetOp() != OpCode::StoreUniform) {
        return false;
    }
    const auto uniform = inst->GetArg<Uniform>(0);
    const auto value = inst->GetArg<Value>(1);
    return uniform.GetType() == ValueType::U8 && value.Def() &&
           value.Def()->GetOp() == OpCode::LoadImm &&
           value.Def()->GetArg<Imm>(0).Get() <= 1;
}

bool HasBackendSubBranchMarker(Block* block) {
    auto terminal = block->GetTerminal();
    auto* branch = boost::get<terminal::If>(&terminal);
    if (!branch || !branch->cond.Def() || branch->cond.Def()->GetUses() != 1) {
        return false;
    }
    const auto condition_op = branch->cond.Def()->GetOp();
    if (condition_op != OpCode::LocalCondSet &&
        condition_op != OpCode::And && condition_op != OpCode::Or) {
        return false;
    }
    if (condition_op == OpCode::LocalCondSet) {
        const auto condition = branch->cond.Def()->GetArg<Cond>(0);
        if (condition != Cond::EQ && condition != Cond::NE &&
            condition != Cond::CS && condition != Cond::CC) {
            return false;
        }
    }
    bool marker = false;
    bool invert = false;
    bool sub_flags = false;
    for (auto& inst : block->GetInstList()) {
        marker |= inst.GetOp() == OpCode::BranchOnlyEdges;
        invert |= inst.GetOp() == OpCode::InvertCarry;
        if (inst.GetOp() == OpCode::SaveFlags) {
            auto value = inst.GetArg<Value>(0);
            sub_flags |= value.Def() && value.Def()->GetOp() == OpCode::Sub;
        }
    }
    return marker && invert && sub_flags;
}

bool HasBackendZeroBranchProof(Block* block) {
    auto terminal = block->GetTerminal();
    auto* branch = boost::get<terminal::If>(&terminal);
    auto* condition = branch ? branch->cond.Def() : nullptr;
    if (!condition || condition->GetOp() != OpCode::LocalCondSet ||
        condition->GetUses() != 1) {
        return false;
    }
    const auto cond = condition->GetArg<Cond>(0);
    if (cond != Cond::EQ && cond != Cond::NE) {
        return false;
    }
    bool zero_test = false;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != OpCode::BranchOnlyFlags) {
            continue;
        }
        const auto value = inst.GetArg<Value>(0);
        zero_test |= value.Def() && value.Def()->GetOp() == OpCode::And &&
                     True(inst.GetArg<Flags>(1) & Flags::Zero);
    }
    return zero_test;
}

bool PreservesRawFCmp(OpCode op) {
    switch (op) {
        case OpCode::LoadUniform:
        case OpCode::StoreUniform:
        case OpCode::GetHostGPR:
        case OpCode::GetHostFPR:
        case OpCode::SetHostGPR:
        case OpCode::SetHostFPR:
        case OpCode::LoadImm:
        case OpCode::AdvancePC:
        case OpCode::BitCast:
        case OpCode::GetOperand:
        case OpCode::Zero:
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64:
        case OpCode::VecFAdd:
        case OpCode::VecFSub:
        case OpCode::VecFMul:
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::BranchOnlyEdges:
            return true;
        default:
            return false;
    }
}

struct BranchOnlyStats {
    u32 candidates{};
    u32 accepted{};
    u32 reject_edge{};
    u32 reject_live{};
    u32 reject_shape{};
};

HIRBlock* FindSuccessor(HIRBlock* block, const Location& location) {
    for (auto* successor : block->GetSuccessors()) {
        if (successor &&
            successor->GetBlock()->GetStartLocation() == location) {
            return successor;
        }
    }
    return nullptr;
}

bool TryFCmpBranchOnly(Block* block,
                       HIRFunction* function,
                       Inst* condition,
                       Inst* edge_marker,
                       const std::vector<Inst*>& insts,
                       size_t producer_begin,
                       size_t producer_advance,
                       size_t cond_index,
                       BranchOnlyStats& stats) {
    auto* fcmp = condition->GetArg<Value>(0).Def();
    size_t fcmp_index = producer_begin;
    while (fcmp_index < producer_advance && insts[fcmp_index] != fcmp) {
        ++fcmp_index;
    }
    if (!fcmp || fcmp->GetOp() != OpCode::VecFCmp ||
        fcmp_index == producer_advance) {
        stats.reject_shape++;
        return false;
    }

    std::vector<Inst*> victims;
    bool published = false;
    for (size_t i = fcmp_index + 1; i < cond_index; ++i) {
        auto* inst = insts[i];
        if (inst->GetOp() == OpCode::PublishFCmpFlags &&
            inst->GetArg<Value>(0).Def() == fcmp) {
            published = true;
            victims.push_back(inst);
            continue;
        }
        if (inst->GetOp() == OpCode::InvertCarry ||
            IsPolarityStoreShape(inst)) {
            victims.push_back(inst);
            continue;
        }
        if (!PreservesRawFCmp(inst->GetOp())) {
            stats.reject_shape++;
            return false;
        }
    }
    if (!published) {
        stats.reject_shape++;
        return false;
    }
    if (edge_marker) {
        victims.push_back(edge_marker);
    }
    for (auto* victim : victims) {
        if (function) {
            function->EraseInst(block, victim);
        } else {
            block->GetInstList().erase(
                    block->GetInstList().iterator_to(*victim));
            delete victim;
        }
    }
    stats.accepted++;
    return true;
}

bool TryBranchOnly(Block* block,
                   HIRBlock* hir_block,
                   HIRFunction* function,
                   const LiveMap& live_in,
                   BranchOnlyStats& stats) {
    auto block_terminal = block->GetTerminal();
    auto* if_term = boost::get<terminal::If>(&block_terminal);
    if (!if_term) {
        return false;
    }

    auto* then_link = boost::get<terminal::LinkBlock>(&if_term->then_);
    auto* else_link = boost::get<terminal::LinkBlock>(&if_term->else_);
    if (!then_link || !else_link) {
        stats.reject_edge++;
        return false;
    }
    stats.candidates++;

    Inst* condition = if_term->cond.Def();
    if (!condition || condition->GetUses() != 1 ||
        (condition->GetOp() != OpCode::LocalCondSet &&
         condition->GetOp() != OpCode::LocalParitySet &&
         condition->GetOp() != OpCode::FCmpCondSet)) {
        stats.reject_shape++;
        return false;
    }

    std::vector<Inst*> insts;
    insts.reserve(block->GetInstList().size());
    for (auto& inst : block->GetInstList()) {
        insts.push_back(&inst);
    }
    size_t cond_index = insts.size();
    for (size_t i = 0; i < insts.size(); ++i) {
        if (insts[i] == condition) {
            cond_index = i;
            break;
        }
    }
    if (cond_index == insts.size()) {
        stats.reject_shape++;
        return false;
    }
    Inst* edge_marker = nullptr;
    for (auto* inst : insts) {
        if (inst->GetOp() == OpCode::BranchOnlyEdges) {
            if (edge_marker) {
                stats.reject_shape++;
                return false;
            }
            edge_marker = inst;
        }
    }
    if (!edge_marker) {
        if (!hir_block) {
            stats.reject_edge++;
            return false;
        }
        auto* then_block = FindSuccessor(hir_block, then_link->next);
        auto* else_block = FindSuccessor(hir_block, else_link->next);
        if (!then_block || !else_block || then_block == else_block ||
            then_block->GetBlock()->GetInstList().empty() ||
            else_block->GetBlock()->GetInstList().empty() ||
            !then_block->GetBlock()->HasTerminal() ||
            !else_block->GetBlock()->HasTerminal()) {
            stats.reject_edge++;
            return false;
        }
        auto then_live = live_in.find(then_block);
        auto else_live = live_in.find(else_block);
        if (then_live == live_in.end() || else_live == live_in.end()) {
            stats.reject_edge++;
            return false;
        }
        if (then_live->second != Flags::None ||
            else_live->second != Flags::None) {
            stats.reject_live++;
            return false;
        }
    }

    size_t producer_end = cond_index;
    while (producer_end > 0 &&
           insts[producer_end - 1]->GetOp() != OpCode::AdvancePC) {
        --producer_end;
    }
    if (producer_end == 0) {
        stats.reject_shape++;
        return false;
    }
    const size_t producer_advance = producer_end - 1;
    size_t producer_begin = producer_advance;
    while (producer_begin > 0 &&
           insts[producer_begin - 1]->GetOp() != OpCode::AdvancePC) {
        --producer_begin;
    }

    if (condition->GetOp() == OpCode::FCmpCondSet) {
        return TryFCmpBranchOnly(
                block, function, condition, edge_marker, insts,
                producer_begin, producer_advance, cond_index, stats);
    }

    std::vector<Inst*> flag_writes;
    std::vector<Inst*> polarity_stores;
    std::vector<Inst*> carry_inversions;
    Inst* primary = nullptr;
    for (size_t i = producer_begin; i < producer_advance; ++i) {
        Inst* inst = insts[i];
        switch (inst->GetOp()) {
            case OpCode::SaveFlags: {
                flag_writes.push_back(inst);
                auto value = inst->GetArg<Value>(0);
                if (value.Def() && IsBranchOnlyProducer(value.Def()->GetOp())) {
                    primary = inst;
                }
                break;
            }
            case OpCode::ClearFlags:
                flag_writes.push_back(inst);
                break;
            case OpCode::StoreUniform: {
                if (IsPolarityStoreShape(inst)) {
                    polarity_stores.push_back(inst);
                }
                break;
            }
            case OpCode::InvertCarry:
                carry_inversions.push_back(inst);
                break;
            case OpCode::TestFlags:
            case OpCode::TestNotFlags:
            case OpCode::GetFlags:
            case OpCode::Adc:
            case OpCode::Sbb:
            case OpCode::SetCarry:
            case OpCode::SetOverflow:
            case OpCode::PublishFCmpFlags:
            case OpCode::PublishSse42StrFlags:
            case OpCode::CondSelect:
            case OpCode::CondSet:
            case OpCode::LocalCondSet:
            case OpCode::LocalParitySet:
            case OpCode::FCmpCondSet:
            case OpCode::Goto:
            case OpCode::NotGoto:
            case OpCode::BindLabel:
                stats.reject_shape++;
                return false;
            default:
                if (IsHelperBoundary(inst->GetOp())) {
                    stats.reject_shape++;
                    return false;
                }
                break;
        }
    }
    if (flag_writes.empty() || polarity_stores.size() > 1) {
        stats.reject_shape++;
        return false;
    }

    const bool parity = condition->GetOp() == OpCode::LocalParitySet;
    Flags required = Flags::None;
    if (!parity) {
        auto maybe_required = ConditionFlags(condition->GetArg<Cond>(0));
        if (!maybe_required || !primary) {
            stats.reject_shape++;
            return false;
        }
        required = *maybe_required;
    } else {
        auto value = condition->GetArg<Value>(0);
        if (!value.Def() || !IsBranchOnlyProducer(value.Def()->GetOp()) ||
            !primary || primary->GetArg<Value>(0).Def() != value.Def()) {
            stats.reject_shape++;
            return false;
        }
    }

    if (!carry_inversions.empty()) {
        const auto value = primary ? primary->GetArg<Value>(0) : Value{};
        const u32 width = value.Def()
                ? GetValueSizeByte(value.Def()->ReturnType())
                : 0;
        const auto right = value.Def() && value.Def()->GetOp() == OpCode::Sub
                ? value.Def()->GetArg<Operand>(1)
                : Operand{};
        auto* right_value = right.GetLeft().IsValue()
                ? right.GetLeft().value.Def()
                : nullptr;
        const auto primary_it = primary
                ? std::find(insts.begin(), insts.end(), primary)
                : insts.end();
        const bool adjacent = primary_it != insts.end() &&
                std::next(primary_it) != insts.end() &&
                *std::next(primary_it) == carry_inversions.front();
        const bool direct_width = function &&
                block->GetStartLocation() !=
                        function->GetFunction()->GetStartLocation() &&
                (width == sizeof(u32) || width == sizeof(u64));
        const bool narrow_load = width <= sizeof(u16) &&
                right.GetRight().Null() && right.GetOp() == OperandOp::Plus &&
                right_value && right_value->GetOp() == OpCode::LoadMemory;
        if (carry_inversions.size() != 1 || flag_writes.size() != 1 ||
            !adjacent || (!direct_width && !narrow_load) ||
            True(required & Flags::Carry)) {
            stats.reject_shape++;
            return false;
        }
    }

    std::unordered_set<Inst*> victims(flag_writes.begin(), flag_writes.end());
    victims.insert(polarity_stores.begin(), polarity_stores.end());
    victims.insert(carry_inversions.begin(), carry_inversions.end());
    if (edge_marker) {
        victims.insert(edge_marker);
    }
    const auto value = primary->GetArg<Value>(0);
    primary->BranchOnlyFlags(value, required);
    victims.erase(primary);
    for (auto* victim : victims) {
        if (function) {
            function->EraseInst(block, victim);
        } else {
            block->GetInstList().erase(
                    block->GetInstList().iterator_to(*victim));
            delete victim;
        }
    }
    stats.accepted++;
    if (EnvFlagsDebug()) {
        SVM_DIAG_FORMAT(Codegen, "[flags-branch-only] block {:#x}: ACCEPT condition={} parity={} "
                   "successors=({:#x},{:#x}) removed={}\n",
                   block->GetStartLocation().Value(),
                   condition->GetOp(), parity,
                   then_link->next.Value(), else_link->next.Value(),
                   victims.size());
    }
    return true;
}

}  // namespace

void FlagsEliminationPass::Run(Block* block, HIRFunction* hir_function,
                               const FeatureSet& features,
                               Flags live_out) {
    auto& inst_list = block->GetInstList();

    if (BranchOnlyEnabled(features) && !hir_function) {
        BranchOnlyStats stats;
        const LiveMap no_live_in;
        TryBranchOnly(block, nullptr, nullptr, no_live_in, stats);
    }
    block->SetDeadEdgeIntegerBranchProof(
            HasBackendSubBranchMarker(block) ||
            HasBackendZeroBranchProof(block));
    // The marker is proof input, never executable IR. Remove it on every
    // conservative rejection as well as on accepted paths.
    for (auto it = inst_list.begin(); it != inst_list.end();) {
        auto* inst = it.operator->();
        ++it;
        if (inst->GetOp() != OpCode::BranchOnlyEdges) {
            continue;
        }
        if (hir_function) {
            hir_function->EraseInst(block, inst);
        } else {
            inst_list.erase(inst_list.iterator_to(*inst));
            delete inst;
        }
    }

    // Bisect switch for deleting carry writes that are overwritten on every
    // in-block path before a read. With the switch off, preserve the old
    // cross-block-conservative handling exactly.
    const bool carry_elim_off = !features.flag_carry_elim;
    const bool has_carry_consumer =
            std::any_of(inst_list.begin(), inst_list.end(),
                        [](const Inst& inst) {
                            return inst.GetOp() == OpCode::Adc ||
                                   inst.GetOp() == OpCode::Sbb;
                        });
    if (carry_elim_off && has_carry_consumer) {
        return;
    }

    Flags needed = live_out;
    // Needed-set captures at bound labels, keyed by the Goto/NotGoto inst
    // whose value the label binds (in-block branches are forward-only in the
    // current frontends; an unseen target falls back to needing everything).
    std::unordered_map<Inst*, Flags> label_needed;
    std::vector<Inst*> victims;
    std::vector<bool> carry_protected;
    if (has_carry_consumer) {
        carry_protected = FlagsCarryRegions::Classify(block);
    }
    size_t carry_region = carry_protected.empty() ? 0 : carry_protected.size() - 1;
    bool crossed_carry_barrier{};

    u32 stat_save{}, stat_save_dead{}, stat_clear{}, stat_clear_dead{}, stat_setcv{},
        stat_setcv_dead{}, stat_shrunk{}, stat_carry_save{}, stat_carry_save_dead{},
        stat_carry_write{}, stat_carry_write_dead{};

    for (auto it = inst_list.rbegin(); it != inst_list.rend(); ++it) {
        Inst& inst = *it;
        if (has_carry_consumer && inst.GetOp() == OpCode::AdvancePC &&
            carry_region != 0) {
            --carry_region;
        }
        if (has_carry_consumer && carry_protected[carry_region]) {
            needed = Flags::All;
            label_needed.clear();
            crossed_carry_barrier = true;
            continue;
        }
        if (crossed_carry_barrier) {
            needed = Flags::All;
            label_needed.clear();
            crossed_carry_barrier = false;
        }
        switch (inst.GetOp()) {
            case OpCode::BranchOnlyFlags:
                // TryBranchOnly already proved both outgoing edges dead and
                // that this producer's only observer is the terminal branch.
                // It is therefore also a full liveness barrier for older
                // architectural flag writes in this unit. Keep the pseudo
                // itself: it tells the backend to leave only host NZCV live.
                needed = Flags::None;
                break;
            case OpCode::SaveFlags: {
                stat_save++;
                const Flags mask = inst.GetArg<Flags>(1);
                // PF and AF are the two x86 status bits with no host
                // equivalent, and they are what makes an arithmetic guest
                // instruction expensive: the arm64 back end spends one BFI on
                // parity (SaveParity) and an EOR/EOR/UBFX/BFI plus a scratch
                // GPR on the auxiliary carry (SaveAuxiliaryCarry), for two bits
                // that only JP/JNP/SETP, LAHF, PUSHF and the BCD instructions
                // ever read.  Measured over the 25 e2e guests plus the bench
                // kernels, CMP alone cost 16.4 host instructions per guest
                // instruction, and CMP/TEST/ADD/SUB/XOR/AND/INC together were
                // ~48% of all emitted IR.
                //
                // The ARM64 emitters now honor every bit in a partial pseudo:
                // arithmetic uses the flag-setting form iff at least one NZCV
                // bit remains, SaveHostFlags/MergeNZCV commit exactly the
                // requested NZCV subset, and AF/PF are independently guarded.
                // Therefore a surviving SaveFlags may be narrowed to precisely
                // the bits live here, instead of retaining dead sibling bits.
                //
                // SVM_FLAG_FULL_ELIM=0 (and the unset default) is the exact
                // pre-W14 behavior: only the already-shipped PF/AF narrowing
                // below is applied. W14 keeps this opt-in because CoreMark A/B
                // found its 76 emitted bytes to be execution-time neutral.
                constexpr Flags kSoftBits = Flags::Parity | Flags::AuxiliaryCarry;
                const bool narrow_off = !features.flag_narrow;
                const bool full_elim_on = features.flag_full_elim;
                const bool writes_carry = True(mask & Flags::Carry);
                // Keep the older carry switch independently exact: when it is
                // off, a C-writing pseudo follows the pre-Gate-B path even if
                // W14 full-bit narrowing is otherwise enabled.
                const bool legacy_carry_path = writes_carry && carry_elim_off;
                Flags narrowed = full_elim_on && !legacy_carry_path
                                         ? (mask & needed)
                                         : (narrow_off
                                                    ? mask
                                                    : (mask & ~(kSoftBits & ~needed)));
                if (full_elim_on && !legacy_carry_path) {
                    // Mul/Div's backend helper materializes C and V as one
                    // coupled x86 overflow result. It can omit both, but cannot
                    // currently commit only one of the pair. All ordinary ALU
                    // emitters below this IR layer support fully independent
                    // NZCV masks.
                    auto* producer = inst.GetArg<Value>(0).Def();
                    if (producer &&
                        (producer->GetOp() == OpCode::Mul ||
                         producer->GetOp() == OpCode::Div) &&
                        True(narrowed & Flags::CV)) {
                        narrowed |= mask & Flags::CV;
                    }
                }
                if (writes_carry) {
                    stat_carry_save++;
                    stat_carry_write++;
                }
                // The bisect-off path is the old Gate B behavior: every
                // SaveFlags(C) survives and only its surviving PF/AF bits kill
                // earlier writers.
                if (legacy_carry_path) {
                    if (narrowed != mask) {
                        inst.SetArg(1, narrowed);
                        stat_shrunk++;
                    }
                    // It does still unconditionally overwrite whichever of
                    // PF/AF remain in the mask, so those are dead for earlier
                    // writers even under the conservative carry rule.
                    needed &= ~(narrowed & kSoftBits);
                    break;
                }
                const Flags live = narrowed & needed;
                if (False(live)) {
                    stat_save_dead++;
                    if (writes_carry) {
                        stat_carry_save_dead++;
                        stat_carry_write_dead++;
                    }
                    victims.push_back(&inst);
                    if (EnvFlagsDebug()) {
                        SVM_DIAG_FORMAT(Codegen, "[flags-elim-dbg] block {:#x}: DELETE SaveFlags mask={} needed={}\n",
                                   block->GetStartLocation().Value(), FlagsString(mask), FlagsString(needed));
                    }
                } else {
                    if (narrowed != mask) {
                        inst.SetArg(1, narrowed);
                        stat_shrunk++;
                    }
                    needed &= ~narrowed;
                }
                break;
            }
            case OpCode::ClearFlags: {
                stat_clear++;
                const Flags mask = inst.GetArg<Flags>(0);
                const bool writes_carry = True(mask & Flags::Carry);
                if (writes_carry) {
                    stat_carry_write++;
                }
                // Bisect-off retains the old unconditional C protection.
                if (writes_carry && carry_elim_off) {
                    break;
                }
                const Flags live = mask & needed;
                if (False(live)) {
                    stat_clear_dead++;
                    if (writes_carry) {
                        stat_carry_write_dead++;
                    }
                    victims.push_back(&inst);
                } else {
                    const bool full_elim_on = features.flag_full_elim;
                    if (full_elim_on && live != mask) {
                        inst.SetArg(0, live);
                        stat_shrunk++;
                    }
                    needed &= ~live;
                }
                break;
            }
            case OpCode::SetCarry: {
                stat_setcv++;
                stat_carry_write++;
                if (!carry_elim_off) {
                    const Flags bit = Flags::Carry;
                    if (False(needed & bit)) {
                        stat_setcv_dead++;
                        stat_carry_write_dead++;
                        victims.push_back(&inst);
                    } else {
                        needed &= ~bit;
                    }
                }
                break;
            }
            case OpCode::SetOverflow: {
                stat_setcv++;
                const Flags bit = Flags::Overflow;
                if (False(needed & bit)) {
                    stat_setcv_dead++;
                    victims.push_back(&inst);
                } else {
                    needed &= ~bit;
                }
                break;
            }
            case OpCode::InvertCarry:
                if (!carry_elim_off && False(needed & Flags::Carry)) {
                    victims.push_back(&inst);
                } else {
                    needed |= Flags::Carry;
                }
                break;
            case OpCode::PublishFCmpFlags:
                // UCOMIS/COMIS define all six observable arithmetic flags
                // (OF/SF/AF clear, CF/PF/ZF from the relation).
                needed &= ~Flags::All;
                break;
            case OpCode::PublishSse42StrFlags: {
                const auto mask = inst.GetArg<Flags>(1);
                const auto live = mask & needed;
                if (False(live)) {
                    victims.push_back(&inst);
                } else {
                    if (live != mask) {
                        inst.SetArg(1, live);
                        stat_shrunk++;
                    }
                    needed &= ~live;
                }
                break;
            }
            case OpCode::TestFlags:
            case OpCode::TestNotFlags:
                needed |= inst.GetArg<Flags>(0);
                break;
            case OpCode::GetFlags:
                // Both backends move the whole flags word, ignoring the mask.
                needed |= Flags::All;
                break;
            case OpCode::Adc:
            case OpCode::Sbb:
                // Native adc/sbc consume the stored carry implicitly.
                needed |= Flags::Carry;
                break;
            case OpCode::CondSelect:
            case OpCode::CondSet:
            case OpCode::LocalCondSet:
                needed |= ConditionFlags(inst.GetArg<Cond>(0))
                                  .value_or(Flags::NZCV);
                break;
            case OpCode::FCmpCondSet:
                // Reads the named VecFCmp relation, not the guest flags word.
                break;
            case OpCode::BindLabel:
                label_needed[inst.GetArg<Value>(0).Def()] |= needed;
                break;
            case OpCode::Goto:
            case OpCode::NotGoto: {
                // The label value's def is this branch instruction itself.
                if (auto target = label_needed.find(&inst); target != label_needed.end()) {
                    needed |= target->second;
                } else {
                    needed |= Flags::All;
                }
                break;
            }
            default:
                break;
        }
    }

    for (auto* victim : victims) {
        if (hir_function) {
            hir_function->EraseInst(block, victim);
        } else {
            inst_list.erase(inst_list.iterator_to(*victim));
            delete victim;
        }
    }

    if (EnvDumpIr() &&
        (stat_save || stat_clear || stat_setcv)) {
        SVM_DIAG_FORMAT(Codegen, "[flags-elim] block {:#x}: SaveFlags {} -> {} (-{}), ClearFlags {} -> {} "
                   "(-{}), SetC/V {} -> {} (-{}), masks narrowed {}, CarrySaveFlags {} -> {} "
                   "(-{}), CarryWrites {} -> {} (-{})\n",
                   block->GetStartLocation().Value(), stat_save, stat_save - stat_save_dead,
                   stat_save_dead, stat_clear, stat_clear - stat_clear_dead, stat_clear_dead,
                   stat_setcv, stat_setcv - stat_setcv_dead, stat_setcv_dead, stat_shrunk,
                   stat_carry_save, stat_carry_save - stat_carry_save_dead,
                   stat_carry_save_dead, stat_carry_write,
                   stat_carry_write - stat_carry_write_dead, stat_carry_write_dead);
        if (EnvDumpIrPost()) {
            SVM_DIAG_FORMAT(Codegen, "--- post-elim block {:#x} ---\n{}\n",
                       block->GetStartLocation().Value(), block->ToString());
        }
    }
}

void FlagsEliminationPass::Run(HIRBuilder* hir_builder, const FeatureSet& features) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func, features);
    }
}

void FlagsEliminationPass::Run(HIRFunction* hir_function,
                               const FeatureSet& features) {
    auto live_in = ComputeFunctionLiveIn(hir_function);
    if (BranchOnlyEnabled(features)) {
        BranchOnlyStats stats;
        for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
            auto* block = hir_block.GetBlock();
            if (!block->GetInstList().empty() && block->HasTerminal()) {
                TryBranchOnly(block, &hir_block, hir_function, live_in, stats);
            }
        }
        if (EnvFlagsDebug()) {
            SVM_DIAG_FORMAT(Codegen, "[flags-branch-only-summary] candidates={} accepted={} "
                       "reject_edge={} reject_live={} reject_shape={}\n",
                       stats.candidates, stats.accepted, stats.reject_edge,
                       stats.reject_live, stats.reject_shape);
        }
    }
    live_in = ComputeFunctionLiveIn(hir_function);
    for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
        const auto live_out = ComputeBlockLiveOut(&hir_block, live_in);
        Run(hir_block.GetBlock(), hir_function, features,
            live_out == Flags::None ? Flags::None : Flags::All);
    }
}

}  // namespace swift::runtime::ir
