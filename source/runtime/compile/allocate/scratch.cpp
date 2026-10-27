#include "runtime/common/perf_stats.h"
#include "runtime/compile/allocate/internal.h"
#include "runtime/compile/allocate/scratch.h"
#include "runtime/compile/analysis/uses.h"

namespace swift::runtime::compile::detail {
bool LinearScan::Verify() {
    VerifyFixedConstraints();
    // Fast path, and the one every unit in the corpus takes: nothing was
    // spilled, so no instruction needs a reload register, and AllocGPR /
    // AllocFPR already refused to drop below `*_reserve` at every mask
    // they recorded. Only an opcode whose budget exceeds that reserve can
    // be short; its prepared site requirements cover that exception.
    if (spill_count == 0) {
        bool ok = true;
        auto check_budgets = [&](const Block* lir_block) {
            for (auto& inst : lir_block->GetInstList()) {
                const auto need = inputs.ScratchFor(inst.Id(), reg_alloc->GetGprs());
                if (need.gpr <= gpr_reserve && need.fpr <= fpr_reserve) {
                    continue;
                }
                if (inst.Id() >= reg_alloc->MapCount()) {
                    continue;
                }
                ok &= static_cast<u32>(reg_alloc->DirtyGPR(inst.Id()).GetClearCount()) +
                                      need.outside_value_pool >=
                              need.gpr &&
                      static_cast<u32>(reg_alloc->DirtyFPR(inst.Id()).GetClearCount()) >= need.fpr;
            }
        };
        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                check_budgets(hir_block->GetBlock());
            }
        } else {
            check_budgets(block);
        }
        return ok;
    }
    bool ok = true;
    auto check_block = [&](const Block* lir_block) {
        u32 last_id = 0;
        for (auto& inst : lir_block->GetInstList()) {
            last_id = std::max<u32>(last_id, inst.Id());
            ok &= CheckInstr(&inst, 0, 0);
        }
        // The block terminal is emitted after the last instruction's
        // TickIR and shares its mask, so any spilled value the terminal
        // reads reloads into that instruction's remaining headroom.
        u32 term_gpr = 0, term_fpr = 0;
        CountTerminalReloads(lir_block->GetTerminal(), term_gpr, term_fpr);
        if ((term_gpr || term_fpr) && last_id < reg_alloc->MapCount()) {
            for (auto& inst : lir_block->GetInstList()) {
                if (inst.Id() == last_id) {
                    ok &= CheckInstr(&inst, term_gpr, term_fpr);
                    break;
                }
            }
        }
    };
    if (function) {
        for (auto* hir_block : function->GetHIRBlocks()) {
            check_block(hir_block->GetBlock());
        }
    } else {
        check_block(block);
    }
    return ok;
}

void LinearScan::CountTerminalReloads(const Terminal& terminal, u32& gpr, u32& fpr) {
    const auto reloads = compile::CountTerminalReloads(*reg_alloc, terminal);
    gpr += reloads.gpr;
    fpr += reloads.fpr;
}

bool LinearScan::CheckInstr(const Inst* inst, u32 extra_gpr, u32 extra_fpr) {
    return CheckInstructionScratch(*reg_alloc,
                                   *inst,
                                   inputs.ScratchFor(inst->Id(), reg_alloc->GetGprs()),
                                   {extra_gpr, extra_fpr});
}
}  // namespace swift::runtime::compile::detail

namespace swift::runtime::compile {
using namespace ir;
namespace {
bool IsSpilled(const AllocationWriter& allocation, ir::Value value) {
    value = ir::ResolveBitCastSource(value);
    if (!value.Defined()) return false;
    ASSERT(value.Id() < allocation.MapCount());
    return allocation.ValueType(value) == AllocationWriter::MEM;
}
}  // namespace
ScratchReloads CountTerminalReloads(const AllocationWriter& allocation, const ir::Terminal& term) {
    ScratchReloads result;
    auto& [gpr, fpr] = result;
    auto count = [&](const Value& value) {
        if (!IsSpilled(allocation, value)) {
            return;
        }
        (ir::IsFloatValueType(ir::ResolveBitCastSource(value).Def()->ReturnType()) ? fpr : gpr)++;
    };
    VisitTerminalValues(term, count);
    return result;
}

bool CheckInstructionScratch(const AllocationWriter& allocation,
                             const ir::Inst& instruction,
                             const ScratchRequirements& need,
                             ScratchReloads extra) {
    const auto* inst = &instruction;
    const u32 id = inst->Id();
    if (id >= allocation.MapCount()) {
        return true;  // no mask was recorded for this id
    }
    u32 reload_gpr = 0;
    u32 reload_fpr = 0;
    // One reload register per DISTINCT spilled value the instruction
    // names -- distinct because JitContext memoizes reloads per
    // (instruction, value).
    StackVector<u32, 8> counted{};
    const auto add_spill = [&](u32 value, bool is_float) {
        if (allocation.ValueType(value) != AllocationWriter::MEM) return;
        if (std::find(counted.begin(), counted.end(), value) != counted.end()) {
            return;
        }
        counted.push_back(value);
        if (is_float) {
            ++reload_fpr;
        } else if (!allocation.HasSpillReload(value, id)) {
            ++reload_gpr;
        }
    };
    const auto add = [&](Value value) {
        value = ResolveBitCastSource(value);
        if (value.Defined()) add_spill(value.Id(), ir::IsFloatValueType(value.Def()->ReturnType()));
    };
    inst->ForEachValue([&](Value value) { add(value); });
    if (inst->HasValue()) {
        if (inst->IsBitCastOperation())
            add(inst->GetArg<Value>(0));
        else
            add_spill(id, ir::IsFloatValueType(inst->ReturnType()));
    }
    auto gprs = allocation.DirtyGPR(id);
    auto fprs = allocation.DirtyFPR(id);
    const u32 scratch_only_gprs = need.outside_value_pool;
    u32 need_gpr = need.gpr + reload_gpr + extra.gpr;
    // Legacy Add/Sub's five-register declaration is the no-spill worst case:
    // tied inputs must be preserved for AF/PF after the destination is
    // overwritten. A spilled operand/result cannot be tied, and its reload
    // register replaces that preservation temporary. Charge the larger of
    // the no-spill peak and the ordinary three-register shape plus reloads
    // instead of adding both mutually-exclusive peaks. Precise pricing
    // excludes those dead preservation arms, so ordinary scratch and
    // reload registers are independent and use the direct sum above.
    if (need.overlap_gpr_reloads) {
        need_gpr = std::max<u32>(need.gpr, need.reload_headroom + reload_gpr) + extra.gpr;
    }
    const u32 need_fpr = need.fpr + reload_fpr + extra.fpr;
    const bool fits = static_cast<u32>(gprs.GetClearCount()) + scratch_only_gprs >= need_gpr &&
                      static_cast<u32>(fprs.GetClearCount()) >= need_fpr;
    if (!fits && GetSvmConfig().ra_diag) {
        SVM_DIAG_PRINT(RegisterAllocation,
                       "[ra-budget] ir=%u op=%u free=%u/%u extra=%u need=%u/%u reload=%u/%u\n",
                       id,
                       static_cast<u32>(inst->GetOp()),
                       static_cast<u32>(gprs.GetClearCount()),
                       static_cast<u32>(fprs.GetClearCount()),
                       scratch_only_gprs,
                       need_gpr,
                       need_fpr,
                       reload_gpr,
                       reload_fpr);
    }
    return fits;
}

}  // namespace swift::runtime::compile
