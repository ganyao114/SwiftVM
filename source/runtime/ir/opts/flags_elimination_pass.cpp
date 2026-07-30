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
#include <unordered_map>
#include <vector>

#include "fmt/format.h"


// Cached diagnostic-env probes: these sit on the per-block path and
// Darwin's getenv() walks `environ` on every call.
static bool EnvOnce(const char* name) { return std::getenv(name) != nullptr; }
static const bool kEnv_dump_ir = EnvOnce("SVM_DUMP_IR");
static const bool kEnv_dump_ir_post = EnvOnce("SVM_DUMP_IR_POST");
static const bool kEnv_flags_debug = EnvOnce("SVM_FLAGS_DEBUG");

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
    for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
        Run(hir_block.GetBlock(), hir_function);
    }
}

}  // namespace swift::runtime::ir
