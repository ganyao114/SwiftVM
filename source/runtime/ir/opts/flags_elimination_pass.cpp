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
//           Adc / Sbb (implicit C), CondSelect (implicit NZCV via host cond)
// The guest flags live in the backend flags register (JIT) / state word
// (interpreter) across blocks, so every bit is live-out of a block.
//
// Removal is safe in both backends: a removed SaveFlags only makes its def
// emit the non-flag-setting instruction form, so no later reader can observe
// it (readers make their bits needed, which keeps the last write). ClearFlags
// remains an independent write even when a sibling SaveFlags is removed.

#include "flags_elimination_pass.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include "fmt/format.h"

namespace swift::runtime::ir {

void FlagsEliminationPass::Run(Block* block, HIRFunction* hir_function) {
    auto& inst_list = block->GetInstList();

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
                // Narrowing the mask in general is unsafe -- the emitter reads
                // the pseudo mask to choose between the flag-setting and plain
                // form of a host instruction, so dropping an NZCV bit changes
                // which instruction is emitted.  PF and AF are exactly the bits
                // that take no part in that decision (`needs_nzcv` tests
                // Flags::NZCV, and both back ends read this same mask), which
                // is what makes narrowing *these two* safe where a general
                // narrowing is not.
                constexpr Flags kSoftBits = Flags::Parity | Flags::AuxiliaryCarry;
                // Bisect switch, mirroring SVM_UNIFORM_DSE / SVM_CONST_CSE.
                static const bool narrow_off = [] {
                    const char* e = std::getenv("SVM_FLAG_NARROW");
                    return e && std::strcmp(e, "0") == 0;
                }();
                const Flags narrowed =
                        narrow_off ? mask : (mask & ~(kSoftBits & ~needed));

                // Never delete a SaveFlags that writes C: carry persists across
                // blocks and a later block's Adc/Sbb may read it. Per-block
                // liveness cannot model that dependency, so such a write also
                // does not "kill" NZCV/OF for earlier writers.
                if (True(mask & Flags::Carry)) {
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
                    victims.push_back(&inst);
                    if (std::getenv("SVM_FLAGS_DEBUG")) {
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
                // Same C protection as SaveFlags.
                if (True(mask & Flags::Carry)) {
                    break;
                }
                const Flags live = mask & needed;
                if (False(live)) {
                    stat_clear_dead++;
                    victims.push_back(&inst);
                } else {
                    // No narrowing (same as SaveFlags).
                    needed &= ~mask;
                }
                break;
            }
            case OpCode::SetCarry:
                // Never delete SetCarry: same cross-block carry reasoning.
                stat_setcv++;
                break;
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
        if (hir_function) {
            hir_function->EraseInst(block, victim);
        } else {
            inst_list.erase(inst_list.iterator_to(*victim));
            delete victim;
        }
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
        Run(hir_block.GetBlock(), hir_function);
    }
}

}  // namespace swift::runtime::ir
