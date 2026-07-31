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

#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fmt/format.h"
#include "runtime/common/variant_util.h"


// Cached diagnostic-env probes: these sit on the per-block path and
// Darwin's getenv() walks `environ` on every call.
static bool EnvOnce(const char* name) { return std::getenv(name) != nullptr; }
static const bool kEnv_dump_ir = EnvOnce("SVM_DUMP_IR");
static const bool kEnv_dump_ir_post = EnvOnce("SVM_DUMP_IR_POST");
static const bool kEnv_flags_debug = EnvOnce("SVM_FLAGS_DEBUG");

namespace swift::runtime::ir {

namespace {

bool BranchOnlyEnabled() {
    const char* env = PerfGetenv("SVM_FLAGS_BRANCH_ONLY");
    return env && std::strcmp(env, "0") != 0;
}

bool IsHelperBoundary(OpCode op) {
    return op == OpCode::CallLambda || op == OpCode::CallLocation ||
           op == OpCode::CallDynamic || op == OpCode::X87Op;
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
                needed |= Flags::NZCV;
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
            if (block->GetInstList().empty() || !block->HasTerminal()) {
                if (live_in[hir_block] != Flags::All) {
                    live_in[hir_block] = Flags::All;
                    changed = true;
                }
                continue;
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
                if (!successor || successor->GetBlock()->GetInstList().empty() ||
                    !successor->GetBlock()->HasTerminal()) {
                    live_out |= Flags::All;
                    continue;
                }
                live_out |= live_in[successor];
            }
            const Flags next = TransferFlagsLiveness(block, live_out);
            if (next != live_in[hir_block]) {
                live_in[hir_block] = next;
                changed = true;
            }
        }
    }
    return live_in;
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

bool IsBranchOnlyProducer(OpCode op) {
    return op == OpCode::Add || op == OpCode::Sub ||
           op == OpCode::And || op == OpCode::Or ||
           op == OpCode::Xor || op == OpCode::AndNot;
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
         condition->GetOp() != OpCode::LocalParitySet)) {
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

    std::vector<Inst*> flag_writes;
    std::vector<Inst*> polarity_stores;
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
                const auto uniform = inst->GetArg<Uniform>(0);
                const auto value = inst->GetArg<Value>(1);
                if (uniform.GetType() == ValueType::U8 && value.Def() &&
                    value.Def()->GetOp() == OpCode::LoadImm &&
                    value.Def()->GetArg<Imm>(0).Get() <= 1) {
                    polarity_stores.push_back(inst);
                }
                break;
            }
            case OpCode::TestFlags:
            case OpCode::TestNotFlags:
            case OpCode::GetFlags:
            case OpCode::Adc:
            case OpCode::Sbb:
            case OpCode::SetCarry:
            case OpCode::SetOverflow:
            case OpCode::InvertCarry:
            case OpCode::PublishFCmpFlags:
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

    std::unordered_set<Inst*> victims(flag_writes.begin(), flag_writes.end());
    victims.insert(polarity_stores.begin(), polarity_stores.end());
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
    if (kEnv_flags_debug) {
        fmt::print("[flags-branch-only] block {:#x}: ACCEPT condition={} parity={} "
                   "successors=({:#x},{:#x}) removed={}\n",
                   block->GetStartLocation().Value(),
                   condition->GetOp(), parity,
                   then_link->next.Value(), else_link->next.Value(),
                   victims.size());
    }
    return true;
}

}  // namespace

void FlagsEliminationPass::Run(Block* block, HIRFunction* hir_function) {
    auto& inst_list = block->GetInstList();

    if (BranchOnlyEnabled() && !hir_function) {
        BranchOnlyStats stats;
        const LiveMap no_live_in;
        TryBranchOnly(block, nullptr, nullptr, no_live_in, stats);
    }
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

    // Adc/Sbb read the carry written by the preceding guest instruction.
    // A block can contain multiple guest instruction regions, each with a
    // SaveFlags boundary. The block-wide backward `needed` set cannot model
    // that boundary: a later Adc/Sbb may make an earlier, unrelated writer
    // appear live (or kill the bits needed by its real carry producer).
    //
    // Keep this conservative until liveness is tracked per guest instruction
    // region. Skipping the entire block means the carry producer and every
    // instruction in its flag-normalization chain remain intact.
    for (const auto& inst : inst_list) {
        if (inst.GetOp() == OpCode::Adc || inst.GetOp() == OpCode::Sbb) {
            return;
        }
    }

    // Bisect switch for deleting carry writes that are overwritten on every
    // in-block path before a read. With the switch off, preserve the old
    // cross-block-conservative handling exactly.
    static const bool carry_elim_off = [] {
        const char* e = PerfGetenv("SVM_FLAG_CARRY_ELIM");
        return e && std::strcmp(e, "0") == 0;
    }();

    Flags needed = Flags::All;  // live-out: flags persist across blocks
    // Needed-set snapshots at bound labels, keyed by the Goto/NotGoto inst
    // whose value the label binds (in-block branches are forward-only in the
    // current frontends; an unseen target falls back to needing everything).
    std::unordered_map<Inst*, Flags> label_needed;
    std::vector<Inst*> victims;

    u32 stat_save{}, stat_save_dead{}, stat_clear{}, stat_clear_dead{}, stat_setcv{},
        stat_setcv_dead{}, stat_shrunk{}, stat_carry_save{}, stat_carry_save_dead{},
        stat_carry_write{}, stat_carry_write_dead{};

    for (auto it = inst_list.rbegin(); it != inst_list.rend(); ++it) {
        Inst& inst = *it;
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
                static const bool narrow_off = [] {
                    const char* e = PerfGetenv("SVM_FLAG_NARROW");
                    return e && std::strcmp(e, "0") == 0;
                }();
                static const bool full_elim_on = [] {
                    const char* e = PerfGetenv("SVM_FLAG_FULL_ELIM");
                    return e && std::strcmp(e, "0") != 0;
                }();
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
                    if (kEnv_flags_debug) {
                        fmt::print("[flags-elim-dbg] block {:#x}: DELETE SaveFlags mask={} needed={}\n",
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
                    static const bool full_elim_on = [] {
                        const char* e = PerfGetenv("SVM_FLAG_FULL_ELIM");
                        return e && std::strcmp(e, "0") != 0;
                    }();
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
                // Representation transform: output C is live exactly when its
                // input C is.  Keep this deliberately conservative; the
                // frontend only emits it between a known producer and ADC/SBB.
                needed |= Flags::Carry;
                break;
            case OpCode::PublishFCmpFlags:
                // UCOMIS/COMIS define all six observable arithmetic flags
                // (OF/SF/AF clear, CF/PF/ZF from the relation).
                needed &= ~Flags::All;
                break;
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
                // Host conditional select / set reads NZCV directly.
                needed |= Flags::NZCV;
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

    if (kEnv_dump_ir &&
        (stat_save || stat_clear || stat_setcv)) {
        fmt::print("[flags-elim] block {:#x}: SaveFlags {} -> {} (-{}), ClearFlags {} -> {} "
                   "(-{}), SetC/V {} -> {} (-{}), masks narrowed {}, CarrySaveFlags {} -> {} "
                   "(-{}), CarryWrites {} -> {} (-{})\n",
                   block->GetStartLocation().Value(), stat_save, stat_save - stat_save_dead,
                   stat_save_dead, stat_clear, stat_clear - stat_clear_dead, stat_clear_dead,
                   stat_setcv, stat_setcv - stat_setcv_dead, stat_setcv_dead, stat_shrunk,
                   stat_carry_save, stat_carry_save - stat_carry_save_dead,
                   stat_carry_save_dead, stat_carry_write,
                   stat_carry_write - stat_carry_write_dead, stat_carry_write_dead);
        if (kEnv_dump_ir_post) {
            fmt::print("--- post-elim block {:#x} ---\n{}\n",
                       block->GetStartLocation().Value(), block->ToString());
        }
    }
}

void FlagsEliminationPass::Run(HIRBuilder* hir_builder) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void FlagsEliminationPass::Run(HIRFunction* hir_function) {
    if (BranchOnlyEnabled()) {
        const auto live_in = ComputeFunctionLiveIn(hir_function);
        BranchOnlyStats stats;
        for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
            auto* block = hir_block.GetBlock();
            if (!block->GetInstList().empty() && block->HasTerminal()) {
                TryBranchOnly(block, &hir_block, hir_function, live_in, stats);
            }
        }
        if (kEnv_flags_debug) {
            fmt::print("[flags-branch-only-summary] candidates={} accepted={} "
                       "reject_edge={} reject_live={} reject_shape={}\n",
                       stats.candidates, stats.accepted, stats.reject_edge,
                       stats.reject_live, stats.reject_shape);
        }
    }
    for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
        Run(hir_block.GetBlock(), hir_function);
    }
}

}  // namespace swift::runtime::ir
