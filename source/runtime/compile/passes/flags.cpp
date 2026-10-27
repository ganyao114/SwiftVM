#include "base/logging.h"
#include "runtime/compile/workspace.h"
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

#include "passes.h"
#include "runtime/compile/analysis/effects.h"
#include "runtime/compile/analysis/flag_flow.h"
#include "runtime/ir/hir_builder.h"

#include <algorithm>

#include "fmt/format.h"


// 诊断开关来自进程级配置快照，不在逐块路径重复扫描 environ。
static bool EnvDumpIr() { return swift::runtime::GetSvmConfig().dump_ir; }
static bool EnvDumpIrPost() { return swift::runtime::GetSvmConfig().dump_ir_post; }
static bool EnvFlagsDebug() { return swift::runtime::GetSvmConfig().flags_debug; }

namespace swift::runtime::compile {
using namespace ir;

namespace {

bool CarryRegionProtected(const Block* block, InstList::const_iterator end, bool& pending) {
    bool consumes_carry{}, writes_carry{};
    // This region has not been edited by the reverse cleanup yet. Each raw
    // region is summarized once, including empty regions between AdvancePCs.
    for (auto it = end; it != block->GetInstList().begin();) {
        const auto& inst = *--it;
        if (inst.GetOp() == OpCode::AdvancePC) break;
        consumes_carry |= inst.GetOp() == OpCode::Adc || inst.GetOp() == OpCode::Sbb;
        switch (inst.GetOp()) {
            case OpCode::SaveFlags:
            case OpCode::BranchOnlyFlags:
                writes_carry |= True(inst.GetArg<Flags>(1) & Flags::Carry);
                break;
            case OpCode::ClearFlags:
                writes_carry |= True(inst.GetArg<Flags>(0) & Flags::Carry);
                break;
            case OpCode::SetCarry:
            case OpCode::PublishFCmpFlags:
                writes_carry = true;
                break;
            case OpCode::PublishSse42StrFlags:
                writes_carry |= True(inst.GetArg<Flags>(1) & Flags::Carry);
                break;
            default:
                break;
        }
    }
    const bool protect = consumes_carry || pending;
    // A later consumer stops at this writer, but a consumer in this very
    // region still needs its preceding producer, even if it also writes C.
    pending = consumes_carry || (pending && !writes_carry);
    return protect;
}

bool BranchOnlyEnabled(const FeatureSet& features) {
    // Default ON after the flip A/B; =0 is the full-materialization rollback.
    return features.flags_branch_only;
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
    const auto& terminal = block->GetTerminal();
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
    const auto& terminal = block->GetTerminal();
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

const HIRBlock* FindSuccessor(const HIRBlock* block, const Location& location) {
    for (auto* successor : block->GetSuccessors()) {
        if (successor &&
            successor->GetBlock()->GetStartLocation() == location) {
            return successor;
        }
    }
    return nullptr;
}

bool TryFCmpBranchOnly(RewriteContext& rewrite,
                       Block* block,
                       const Inst* condition,
                       const Inst* edge_marker,
                       InstList::const_iterator producer_begin,
                       InstList::const_iterator producer_advance,
                       InstList::const_iterator condition_it) {
    const auto* fcmp = condition->GetArg<Value>(0).Def();
    auto fcmp_it = producer_begin;
    while (fcmp_it != producer_advance && &*fcmp_it != fcmp) ++fcmp_it;
    if (!fcmp || fcmp->GetOp() != OpCode::VecFCmp || fcmp_it == producer_advance) {
        return false;
    }
    bool published = false;
    for (auto it = std::next(fcmp_it); it != condition_it; ++it) {
        if (it->GetOp() == OpCode::PublishFCmpFlags && it->GetArg<Value>(0).Def() == fcmp) {
            published = true;
            continue;
        }
        if (it->GetOp() == OpCode::InvertCarry || IsPolarityStoreShape(&*it)) continue;
        if (!PreservesRawFCmp(it->GetOp())) {
            return false;
        }
    }
    if (!published) {
        return false;
    }
    // Proof completes before edits. These endpoints are retained; deletion
    // notifications only queue DCE, so advancing first keeps the cursor live.
    for (auto it = std::next(fcmp_it); it != condition_it;) {
        const auto* inst = &*it++;
        if ((inst->GetOp() == OpCode::PublishFCmpFlags && inst->GetArg<Value>(0).Def() == fcmp) ||
            inst->GetOp() == OpCode::InvertCarry || IsPolarityStoreShape(inst))
            rewrite.Erase(block, inst);
    }
    if (edge_marker) rewrite.Erase(block, edge_marker);
    return true;
}

const terminal::If* BranchOnlyCandidate(const Block* block) {
    const auto* branch = boost::get<terminal::If>(&block->GetTerminal());
    if (!branch || !boost::get<terminal::LinkBlock>(&branch->then_) ||
        !boost::get<terminal::LinkBlock>(&branch->else_))
        return nullptr;
    const auto* condition = branch->cond.Def();
    if (!condition || condition->GetUses() != 1 ||
        (condition->GetOp() != OpCode::LocalCondSet &&
         condition->GetOp() != OpCode::LocalParitySet && condition->GetOp() != OpCode::FCmpCondSet))
        return nullptr;
    return branch;
}

bool TryBranchOnly(RewriteContext& rewrite,
                   Block* block,
                   const HIRBlock* hir_block,
                   HIRFunction* function,
                   const FlagFlow* flow,
                   const terminal::If& branch) {
    const auto* then_link = boost::get<terminal::LinkBlock>(&branch.then_);
    const auto* else_link = boost::get<terminal::LinkBlock>(&branch.else_);
    const Inst* condition = branch.cond.Def();
    const auto& insts = block->GetInstList();
    auto condition_it = insts.end();
    const Inst* edge_marker{};
    // Locate both identities in one raw walk, without copying the block.
    for (auto it = insts.begin(); it != insts.end(); ++it) {
        if (&*it == condition) condition_it = it;
        if (it->GetOp() == OpCode::BranchOnlyEdges) {
            if (edge_marker) {
                return false;
            }
            edge_marker = &*it;
        }
    }
    if (condition_it == insts.end()) {
        return false;
    }
    const auto successor_demand = [&](const Location& target) -> std::optional<Flags> {
        if (!hir_block || !flow) return std::nullopt;
        const auto* successor = FindSuccessor(hir_block, target);
        if (!successor || successor->GetInstList().empty() ||
            !successor->GetBlock()->HasTerminal()) {
            return std::nullopt;
        }
        return flow->LiveIn(successor);
    };
    const auto then_live = successor_demand(then_link->next);
    const auto else_live = successor_demand(else_link->next);
    // A frontend marker may prove undecoded successors, but cannot override
    // demand from a decoded HIR path, including a fault before its first write.
    if ((then_live && *then_live != Flags::None) ||
        (else_live && *else_live != Flags::None)) {
        return false;
    }
    if (!edge_marker && (!then_live || !else_live || then_link->next == else_link->next)) {
        return false;
    }

    auto producer_end = condition_it;
    while (producer_end != insts.begin() && std::prev(producer_end)->GetOp() != OpCode::AdvancePC)
        --producer_end;
    if (producer_end == insts.begin()) {
        return false;
    }
    const auto producer_advance = std::prev(producer_end);
    auto producer_begin = producer_advance;
    while (producer_begin != insts.begin() &&
           std::prev(producer_begin)->GetOp() != OpCode::AdvancePC)
        --producer_begin;

    if (condition->GetOp() == OpCode::FCmpCondSet)
        return TryFCmpBranchOnly(rewrite,
                                 block,
                                 condition,
                                 edge_marker,
                                 producer_begin,
                                 producer_advance,
                                 condition_it);

    size_t flag_writes{}, polarity_stores{}, carry_inversions{};
    const Inst* first_inversion{};
    auto primary_it = insts.end();
    const Inst* primary = nullptr;
    for (auto it = producer_begin; it != producer_advance; ++it) {
        const Inst* inst = &*it;
        switch (inst->GetOp()) {
            case OpCode::SaveFlags: {
                ++flag_writes;
                auto value = inst->GetArg<Value>(0);
                if (value.Def() && IsBranchOnlyProducer(value.Def()->GetOp())) {
                    primary = inst;
                    primary_it = it;
                }
                break;
            }
            case OpCode::ClearFlags:
                ++flag_writes;
                break;
            case OpCode::StoreUniform: {
                if (IsPolarityStoreShape(inst)) {
                    ++polarity_stores;
                }
                break;
            }
            case OpCode::InvertCarry:
                if (!carry_inversions++) first_inversion = inst;
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

                return false;
            default:
                if (True(inst->GetEffects() & IREffect::Call)) {
                    return false;
                }
                break;
        }
    }
    if (flag_writes == 0 || polarity_stores > 1) {
        return false;
    }

    const bool parity = condition->GetOp() == OpCode::LocalParitySet;
    Flags required = Flags::None;
    if (!parity) {
        auto maybe_required = ConditionFlags(condition->GetArg<Cond>(0));
        if (!maybe_required || !primary) {
            return false;
        }
        required = *maybe_required;
    } else {
        auto value = condition->GetArg<Value>(0);
        if (!value.Def() || !IsBranchOnlyProducer(value.Def()->GetOp()) ||
            !primary || primary->GetArg<Value>(0).Def() != value.Def()) {
            return false;
        }
    }

    // Match the actual arithmetic in source order. A read before it cannot
    // observe its flags, but a later fault before the guest boundary can.
    const auto* arithmetic = primary->GetArg<Value>(0).Def();
    bool seen_arithmetic = false;
    for (auto it = producer_begin; it != producer_advance; ++it) {
        const auto* inst = &*it;
        if (inst == arithmetic) {
            seen_arithmetic = true;
        } else if (seen_arithmetic && True(inst->GetEffects() & IREffect::MayFault)) {
            return false;
        }
    }
    if (!seen_arithmetic) return false;

    if (carry_inversions) {
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
        const bool adjacent = primary_it != insts.end() && std::next(primary_it) != insts.end() &&
                              &*std::next(primary_it) == first_inversion;
        const bool direct_width = function &&
                block->GetStartLocation() !=
                        function->GetFunction()->GetStartLocation() &&
                (width == sizeof(u32) || width == sizeof(u64));
        const bool narrow_load = width <= sizeof(u16) &&
                right.GetRight().Null() && right.GetOp() == OperandOp::Plus &&
                right_value && right_value->GetOp() == OpCode::LoadMemory;
        if (carry_inversions != 1 || flag_writes != 1 || !adjacent ||
            (!direct_width && !narrow_load) || True(required & Flags::Carry)) {
            return false;
        }
    }

    const auto removed =
            flag_writes + polarity_stores + carry_inversions + (edge_marker ? 1 : 0) - 1;
    const auto value = primary->GetArg<Value>(0);
    rewrite.Change(block, primary, [&](Inst& target) { target.BranchOnlyFlags(value, required); });
    // Opcode categories are disjoint and each raw node occurs once. Erase in
    // source order after full qualification; no dedup set or snapshot escapes.
    for (auto it = producer_begin; it != producer_advance;) {
        const auto* inst = &*it++;
        if (inst != primary &&
            (inst->GetOp() == OpCode::SaveFlags || inst->GetOp() == OpCode::ClearFlags ||
             inst->GetOp() == OpCode::InvertCarry || IsPolarityStoreShape(inst)))
            rewrite.Erase(block, inst);
    }
    if (edge_marker) rewrite.Erase(block, edge_marker);

    if (EnvFlagsDebug()) {
        SVM_DIAG_FORMAT(Codegen,
                        "[flags-branch-only] block {:#x}: ACCEPT condition={} parity={} "
                        "successors=({:#x},{:#x}) removed={}\n",
                        block->GetStartLocation().Value(),
                        condition->GetOp(),
                        parity,
                        then_link->next.Value(),
                        else_link->next.Value(),
                        removed);
    }
    return true;
}

}  // namespace

static void CleanBlockFlags(RewriteContext& rewrite, Block* block, HIRFunction* hir_function,
                            const FeatureSet& features, Flags live_out, bool branch_only,
                            FlagFlow& flow) {
    auto& inst_list = block->GetInstList();

    if (branch_only && BranchOnlyEnabled(features) && !hir_function) {
        if (const auto* candidate = BranchOnlyCandidate(block))
            TryBranchOnly(rewrite, block, nullptr, nullptr, nullptr, *candidate);
    }
    if (branch_only) block->SetDeadEdgeIntegerBranchProof(
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
        rewrite.Erase(block, inst);
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
    // Snapshots only become visible when their binding is visited in this
    // reverse walk. A roster entry alone cannot prove a forward target.
    flow.BeginBlock(block);
    bool pending_carry{};
    bool carry_protected =
            has_carry_consumer && CarryRegionProtected(block, inst_list.end(), pending_carry);
    bool crossed_carry_barrier{};

    u32 stat_save{}, stat_save_dead{}, stat_clear{}, stat_clear_dead{}, stat_setcv{},
        stat_setcv_dead{}, stat_shrunk{}, stat_carry_save{}, stat_carry_save_dead{},
        stat_carry_write{}, stat_carry_write_dead{};

    for (auto cursor = inst_list.end(); cursor != inst_list.begin();) {
        const auto after = cursor;
        cursor = std::prev(cursor);
        const Inst& inst = *cursor;
        if (rewrite.EraseDead(block, &inst)) {
            cursor = after;
            continue;
        }
        bool remove = false;
        if (has_carry_consumer && inst.GetOp() == OpCode::AdvancePC) {
            carry_protected = CarryRegionProtected(block, cursor, pending_carry);
        }
        if (carry_protected) {
            needed = Flags::All;
            flow.ForgetLabels();
            crossed_carry_barrier = true;
            continue;
        }
        if (crossed_carry_barrier) {
            needed = Flags::All;
            flow.ForgetLabels();
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
                        rewrite.SetArg(block, &inst, 1, narrowed);
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
                    remove = true;
                    if (EnvFlagsDebug()) {
                        SVM_DIAG_FORMAT(Codegen, "[flags-elim-dbg] block {:#x}: DELETE SaveFlags mask={} needed={}\n",
                                   block->GetStartLocation().Value(), FlagsString(mask), FlagsString(needed));
                    }
                } else {
                    if (narrowed != mask) {
                        rewrite.SetArg(block, &inst, 1, narrowed);
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
                    remove = true;
                } else {
                    const bool full_elim_on = features.flag_full_elim;
                    if (full_elim_on && live != mask) {
                        rewrite.SetArg(block, &inst, 0, live);
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
                        remove = true;
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
                    remove = true;
                } else {
                    needed &= ~bit;
                }
                break;
            }
            case OpCode::InvertCarry:
                if (!carry_elim_off && False(needed & Flags::Carry)) {
                    remove = true;
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
                    remove = true;
                } else {
                    if (live != mask) {
                        rewrite.SetArg(block, &inst, 1, live);
                        stat_shrunk++;
                    }
                    needed &= ~live;
                }
                break;
            }
            case OpCode::TestFlags:
            case OpCode::TestNotFlags:
            case OpCode::GetFlags:
            case OpCode::Adc:
            case OpCode::Sbb:
            case OpCode::CondSelect:
            case OpCode::CondSet:
            case OpCode::LocalCondSet:
                needed |= InstructionFlagAccess(inst).read;
                break;
            case OpCode::FCmpCondSet:
                // Reads the named VecFCmp relation, not the guest flags word.
                break;
            case OpCode::BindLabel:
                flow.RememberLabel(inst.GetArg<Value>(0).Def(), needed);
                break;
            case OpCode::Goto:
            case OpCode::NotGoto: {
                // The label value's def is this branch instruction itself.
                if (const auto target = flow.LabelDemand(&inst)) {
                    needed |= *target;
                } else {
                    needed |= Flags::All;
                }
                break;
            }
            default:
                needed |= InstructionFlagAccess(inst).read;
                break;
        }
        if (remove) {
            rewrite.Erase(block, &inst);
            cursor = after;
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

void EliminateFlags(RewriteContext& rewrite, const FeatureSet& features, Flags live_out) {
    rewrite.CollectDead();
    bool first = rewrite.BeginFlags();
    if (rewrite.IsSingleBlock()) {
        FlagFlow flow{rewrite.Blocks().front()};
        do {
            rewrite.ClearFlagChanges();
            CleanBlockFlags(rewrite, rewrite.Blocks().front(), rewrite.Function(),
                            features, live_out, first, flow);
            first = false;
            rewrite.CollectDead();
        } while (rewrite.HasFlagChanges());
        return;
    }
    auto* function = rewrite.Function();
    FlagFlow flow{function};
    if (first && BranchOnlyEnabled(features)) {
        bool demand_prepared{};
        for (auto& current : function->GetHIRBlocksRPO()) {
            auto* block = function->EditBlock(&current).GetBlock();
            if (!block->GetInstList().empty() && block->HasTerminal()) {
                if (const auto* candidate = BranchOnlyCandidate(block)) {
                    // Keep the solver in this outer frame and freeze demand
                    // before any branch-only rewrite, including marked ones.
                    if (!demand_prepared) {
                        flow.ComputeLiveIn();
                        demand_prepared = true;
                    }
                    TryBranchOnly(rewrite, block, &current, function, &flow, *candidate);
                }
            }
        }
    }
    // Deleting a dead reader in a later block can make a predecessor's flags
    // writer dead. Recompute the actual CFG demand until notifications stop.
    do {
        rewrite.ClearFlagChanges();
        flow.ComputeLiveIn();
        for (auto& current : function->GetHIRBlocksRPO()) {
            const auto out = flow.LiveOut(&current);
            CleanBlockFlags(rewrite,
                            function->EditBlock(&current).GetBlock(),
                            function,
                            features,
                            out == Flags::None ? Flags::None : Flags::All,
                            first,
                            flow);
        }
        first = false;
        rewrite.CollectDead();
    } while (rewrite.HasFlagChanges());
}

}  // namespace swift::runtime::compile
