//
// Created by 甘尧 on 2024/6/26.
//
// Backward flag liveness, in the spirit of FEX's
// RedundantFlagCalculationElimination: walk the block from the end, track the
// set of guest flag bits that are still needed, and drop (or narrow) flag
// writes whose bits are all rewritten later before any read.
//
// Flag model in this IR (see ir.inc):
//   writes: SaveFlags(value, mask) / ClearFlags(mask) / SetCarry / SetOverflow
//   reads:  TestFlags / TestNotFlags (mask), GetFlags (whole register),
//           Adc / Sbb (implicit C), CondSelect (implicit NZCV via host cond)
// The guest flags live in the backend flags register (JIT) / state word
// (interpreter) across blocks, so every bit is live-out of a block.
//
// Removal is safe in both backends: a removed SaveFlags only makes its def
// emit the non-flag-setting instruction form, so no later reader can observe
// it (readers make their bits needed, which keeps the last write). Narrowing
// a mask never changes JIT codegen for NZCV (the lazy merge is whole-NZCV)
// and is per-bit exact in the interpreter.

#include "flags_elimination_pass.h"

#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "fmt/format.h"

namespace swift::runtime::ir {

void FlagsEliminationPass::Run(Block* block) {
    auto& inst_list = block->GetInstList();

    Flags needed = Flags::All;  // live-out: flags persist across blocks
    // Needed-set snapshots at bound labels, keyed by the Goto/NotGoto inst
    // whose value the label binds (in-block branches are forward-only in the
    // current frontends; an unseen target falls back to needing everything).
    std::unordered_map<Inst*, Flags> label_needed;
    std::vector<Inst*> victims;

    u32 stat_save{}, stat_save_dead{}, stat_clear{}, stat_clear_dead{}, stat_setcv{},
        stat_setcv_dead{}, stat_shrunk{};

    for (auto it = inst_list.rbegin(); it != inst_list.rend(); ++it) {
        Inst& inst = *it;
        switch (inst.GetOp()) {
            case OpCode::SaveFlags: {
                stat_save++;
                const Flags mask = inst.GetArg<Flags>(1);
                const Flags live = mask & needed;
                if (False(live)) {
                    stat_save_dead++;
                    victims.push_back(&inst);
                } else {
                    if (live != mask) {
                        inst.SetArg(1, live);
                        stat_shrunk++;
                    }
                    needed &= ~mask;
                }
                break;
            }
            case OpCode::ClearFlags: {
                stat_clear++;
                const Flags mask = inst.GetArg<Flags>(0);
                const Flags live = mask & needed;
                if (False(live)) {
                    stat_clear_dead++;
                    victims.push_back(&inst);
                } else {
                    if (live != mask) {
                        inst.SetArg(0, live);
                        stat_shrunk++;
                    }
                    needed &= ~mask;
                }
                break;
            }
            case OpCode::SetCarry:
            case OpCode::SetOverflow: {
                stat_setcv++;
                const Flags bit = inst.GetOp() == OpCode::SetCarry ? Flags::Carry
                                                                   : Flags::Overflow;
                if (False(needed & bit)) {
                    stat_setcv_dead++;
                    victims.push_back(&inst);
                } else {
                    needed &= ~bit;
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
                // Host conditional select reads NZCV directly.
                needed |= Flags::NZCV;
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
        inst_list.erase(inst_list.iterator_to(*victim));
        delete victim;
    }

    if (std::getenv("SVM_DUMP_IR") &&
        (stat_save || stat_clear || stat_setcv)) {
        fmt::print("[flags-elim] block {:#x}: SaveFlags {} -> {} (-{}), ClearFlags {} -> {} "
                   "(-{}), SetC/V {} -> {} (-{}), masks narrowed {}\n",
                   block->GetStartLocation().Value(), stat_save, stat_save - stat_save_dead,
                   stat_save_dead, stat_clear, stat_clear - stat_clear_dead, stat_clear_dead,
                   stat_setcv, stat_setcv - stat_setcv_dead, stat_setcv_dead, stat_shrunk);
        if (std::getenv("SVM_DUMP_IR_POST")) {
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
    for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
        Run(hir_block.GetBlock());
    }
}

}  // namespace swift::runtime::ir
