//
// Created by 甘尧 on 2023/12/6.
//

#include "register_alloc_pass.h"
#include "linear_scan_allocator.h"
#include "register_alloc_internal.h"
#include "register_alloc_spill_reload.h"
#include <cstdio>
#include "base/logging.h"
#include "runtime/backend/arm64/pshufd_direct.h"
#include "runtime/common/perf_stats.h"

namespace swift::runtime::ir {

// Scratch headroom the linear scan must never hand out to a value.
//
// JitContext::GetTmpX takes a scratch register by picking the first register
// NOT set in this pass's per-instruction active mask, and there is no release
// mechanism -- scratch is recycled only at the next TickIR. If the scan fills
// every allocatable GPR, the active mask is all ones, GetTmpX has nothing to
// return and panics ("No free temporary GPR"). That makes spilling
// self-defeating: the very reload the spill requires (JitContext::SpillGPR)
// asks for scratch, so the first spilled value guarantees the panic.
// SVM_FUNC_BASE=0 on x87_bench_x86_64 and x87_topvirt_stress_x86_64 hit
// exactly this and aborted the guest.
//
// The headroom is therefore not a tuning knob but a correctness obligation.
// A previous fixed reserve of 4 was below the real peak of 8 (the vector
// NaN-fixup path behind VecFAdd/VecFMul/X87Op) and survived only because those
// opcodes happened never to appear in a saturated unit -- luck, not a
// guarantee.
//
// It is now derived and *checked* instead of tuned:
//
//   backend::ScratchBudget declares, per opcode, how much scratch its emitter
//   may hold at once (JitContext asserts nobody exceeds their declaration), and
//   after allocating, this pass VERIFIES its own output: for every instruction
//   it re-reads the mask it recorded and confirms the free-register count
//   covers that instruction's budget plus one reload register for each spilled
//   value the instruction names. A unit that fails is re-allocated with a
//   larger reserve.
//
// Verifying beats reserving up front because the reserve is a whole-unit
// property while the demand is per-instruction. Reserving the unit maximum
// would charge every instruction of a function for the one VecFAdd inside it:
// measured on this corpus that turned basic_coverage_smoke from zero spills
// into 777. Verification only escalates the units where high demand actually
// coincides with high pressure -- which, on the whole corpus, is none.

Value ResolveBitCastSource(Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<Value>(0);
    }
    return value;
}


VRegisterAllocator::VRegisterAllocator(Block* block)
        : block(block), live_interval(), active_lives() {
}

void VRegisterAllocator::AllocateRegisters() {
    CollectLiveIntervals();
    // Step 2: Sort live intervals
    std::sort(live_interval.begin(), live_interval.end());

    // Step 3: Alloc Registers
    for (auto& interval : live_interval) {
        ExpireOldIntervals(interval);

        active_lives.push_back(interval);
        AllocVReg(interval);
    }
}

void VRegisterAllocator::ExpireOldIntervals(LiveInterval& current) {
    for (auto it = active_lives.begin(); it != active_lives.end();) {
        if (it->end < current.start) {
            active_v_regs[it->inst->VirRegID()] = false;
            if (IsFloatValue(it->inst)) {
                active_v_regs[it->inst->VirRegID() + 1] = false;
            }
            it = active_lives.erase(it);  // Remove expired intervals
        } else {
            ++it;
        }
    }
}

bool VRegisterAllocator::IsFloatValue(Inst* inst) {
    auto value_type = inst->ReturnType();
    return value_type >= ValueType::V8 && value_type <= ValueType::V256;
}

void VRegisterAllocator::GrowVRegs(u32 new_item_size) {
    active_v_regs_cursor = active_v_regs.size();
    active_v_regs.resize(active_v_regs_cursor + new_item_size);
}

void VRegisterAllocator::AllocVReg(LiveInterval& interval) {
    auto is_float = IsFloatValue(interval.inst);
    auto slot_size = is_float ? 2 : 1;
    if (is_float) {
        s32 slot{-1};
        for (int i = 0; i + 1 < active_v_regs.size(); i += 2) {
            if (!active_v_regs[i] && !active_v_regs[i + 1]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            slot = active_v_regs.size();
            GrowVRegs(slot_size);
        }
        interval.inst->SetVirReg(slot);
        active_v_regs[slot] = true;
    } else {
        auto itr = std::find(active_v_regs.begin(), active_v_regs.end(), false);
        if (itr != active_v_regs.end()) {
            u16 slot = std::distance(active_v_regs.begin(), itr);
            active_v_regs[slot] = true;
            interval.inst->SetVirReg(slot);
        } else {
            // grow stack
            u16 slot = active_v_regs_cursor;
            GrowVRegs(slot_size);
            active_v_regs[slot] = true;
            interval.inst->SetVirReg(slot);
        }
    }
}

void VRegisterAllocator::CollectLiveIntervals() {
    StackVector<u16, 64> use_end{};
    use_end.resize(block->GetInstList().size());
    for (auto& instr : block->GetInstList()) {
        auto values = instr.GetValues();
        for (auto &value : values) {
            auto &end = use_end[value.Id()];
            end = std::max(end, instr.Id());
        }
    }
    for (auto& instr : block->GetInstList()) {
        if (instr.HasValue()) {
            auto start = instr.Id();
            auto end = use_end[start];
            live_interval.push_back({&instr, start, end});
        }
    }
}

void RegisterAllocPass::Run(HIRBuilder* hir_builder, backend::RegAlloc* reg_alloc,
                            const FeatureSet& features) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func, reg_alloc, features);
    }
}

#include "register_alloc_verified.inc"

void RegisterAllocPass::Run(HIRFunction* hir_function,
                            backend::RegAlloc* reg_alloc,
                            const FeatureSet& features) {
    RunWithScalarInsert(hir_function, reg_alloc, false, features);
}

void RegisterAllocPass::RunWithScalarInsert(HIRFunction* hir_function,
                                            backend::RegAlloc* reg_alloc,
                                            bool scalar_insert,
                                            const FeatureSet& features) {
    const bool single_block_fast_path = features.ra_1blk;
    const bool use_fast_path =
            single_block_fast_path && hir_function->GetHIRBlocksRPO().size() == 1;
    RunVerified(hir_function, reg_alloc, features, use_fast_path, scalar_insert,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocPass::Run(HIRFunction* hir_function,
                            backend::RegAlloc* reg_alloc,
                            bool single_block_fast_path,
                            const FeatureSet& features) {
    const bool use_fast_path =
            single_block_fast_path && hir_function->GetHIRBlocksRPO().size() == 1;
    RunVerified(hir_function, reg_alloc, features, use_fast_path, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocPass::Run(ir::Block* block,
                            backend::RegAlloc* reg_alloc,
                            bool scalar_insert,
                            const FeatureSet& features) {
    RunVerified(block, reg_alloc, features, false, scalar_insert,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}


void VRegisterAllocPass::Run(ir::Block* block) {
    VRegisterAllocator allocator{block};
    allocator.AllocateRegisters();
}

}  // namespace swift::runtime::ir
