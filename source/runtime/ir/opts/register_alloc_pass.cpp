//
// Created by 甘尧 on 2023/12/6.
//

#include "register_alloc_pass.h"

namespace swift::runtime::ir {

struct LiveInterval {
    Inst* inst{};
    u32 start{};
    u32 end{};

    bool operator<(const LiveInterval& other) const {
        if (start == other.start) {
            return end < other.end;
        } else {
            return start < other.start;
        }
    }
};

class LinearScanAllocator {
public:
    explicit LinearScanAllocator(HIRFunction* function, backend::RegAlloc* alloc)
            : function(function), block(), reg_alloc(alloc), live_interval(), active_lives() {
        active_gprs = alloc->GetGprs();
        active_fprs = alloc->GetFprs();
        live_interval.reserve(function->MaxInstrCount());
    }

    explicit LinearScanAllocator(Block* block, backend::RegAlloc* alloc)
            : function(), block(block), reg_alloc(alloc), live_interval(), active_lives() {
        active_gprs = alloc->GetGprs();
        active_fprs = alloc->GetFprs();
        live_interval.reserve(block->GetInstList().size());
    }

    void AllocateRegisters() {
        // Step 1: Collect live intervals
        if (function) {
            CollectLiveIntervals(function);
        } else {
            CollectLiveIntervals(block);
        }

        // Step 2: Sort live intervals
        std::sort(live_interval.begin(), live_interval.end());

        // Step 3: Alloc Registers
        for (auto& interval : live_interval) {
            ExpireOldIntervals(interval);

            if (!IsFloatValue(interval.inst)) {
                if (auto alloc = AllocGPR(); alloc >= 0) {
                    active_lives.push_back(interval);
                    reg_alloc->MapRegister(interval.inst->Id(), HostGPR{(u16)alloc});
                } else {
                    SpillAtInterval(interval);
                }
            } else {
                if (auto alloc = AllocFPR(); alloc >= 0) {
                    active_lives.push_back(interval);
                    reg_alloc->MapRegister(interval.inst->Id(), HostFPR{(u16)alloc});
                } else {
                    SpillAtInterval(interval);
                }
            }
            reg_alloc->SetActiveRegs(interval.inst->Id(), active_fprs, active_gprs);
        }
    }

private:
    void CollectLiveIntervals(HIRFunction* hir_function) {
        for (auto& hir_value : hir_function->GetHIRValues()) {
            auto instr = hir_value.value.Def();
            if (instr->IsGetHostRegOperation()) {
                auto is_float = instr->GetOp() == OpCode::GetHostFPR;
                auto host_index = instr->GetArg<Imm>(0).Get();
                if (is_float) {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostFPR{(u16) host_index});
                } else {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostGPR{(u16) host_index});
                }
                continue;
            }
            if (instr->IsBitCastOperation()) {
                auto from = instr->GetArg<Value>(0);
                reg_alloc->MapReference(from.Id(), instr->Id());
                continue;
            }
            auto start = hir_value.GetOrderId();
            u32 end{hir_value.GetOrderId()};
            std::for_each(hir_value.uses.begin(), hir_value.uses.end(), [&end](auto& use) {
                end = std::max(end, (u32) use.inst->Id());
            });
            if (auto inst = hir_value.value.Def(); inst->IsPseudoOperation()) {
                start = inst->GetArg<Value>(0).Id();
            }
            live_interval.push_back({hir_value.value.Def(), start, end});
        }
    }

    void CollectLiveIntervals(Block* lir_block) {
        ASSERT_MSG(lir_block, "block == null");
        ASSERT_MSG(!lir_block->IsEmptyBlock(), "block is empty");
        StackVector<u16, 64> use_end{};
        Map<u16, u8> set_value_uses{};
        use_end.resize(lir_block->GetInstList().size());
        for (auto& instr : lir_block->GetInstList()) {
            if (instr.IsGetHostRegOperation()) {
                auto is_float = instr.GetOp() == OpCode::GetHostFPR;
                auto host_index = instr.GetArg<Imm>(0).Get();
                if (is_float) {
                    reg_alloc->MapRegister(instr.Id(), HostFPR{(u16) host_index});
                } else {
                    reg_alloc->MapRegister(instr.Id(), HostGPR{(u16) host_index});
                }
            } else if (instr.IsBitCastOperation()) {
                auto from = instr.GetArg<Value>(0);
                reg_alloc->MapReference(from.Id(), instr.Id());
            } else if (instr.IsSetHostRegOperation()) {
                auto value = instr.GetArg<Value>(0);
                auto host_index = instr.GetArg<Imm>(1).Get();
                auto is_float = instr.GetOp() == OpCode::SetHostFPR;
                auto use_count = value.Def()->GetUses();
                ASSERT(use_count > 0);
                if (use_count == 1) {
                    // Well, Only this
                    if (is_float) {
                        reg_alloc->MapRegister(value.Id(), HostFPR{(u16) host_index});
                    } else {
                        reg_alloc->MapRegister(value.Id(), HostGPR{(u16) host_index});
                    }
                    continue;
                } else {
                    if (auto itr = set_value_uses.find(value.Id()); itr != set_value_uses.end()) {
                        auto uses = itr->second;
                        if (--uses == 0) {
                            if (is_float) {
                                reg_alloc->MapRegister(value.Id(), HostFPR{(u16) host_index});
                            } else {
                                reg_alloc->MapRegister(value.Id(), HostGPR{(u16) host_index});
                            }
                        } else {
                            set_value_uses[value.Id()] = uses;
                        }
                    } else {
                        set_value_uses[value.Id()] = --use_count;
                    }
                    continue;
                }
            } else {
                auto values = instr.GetValues();
                for (auto& value : values) {
                    if (value.Def()->IsBitCastOperation()) {
                        auto src = value.Def()->GetArg<Value>(0);
                        auto& end = use_end[src.Id()];
                        end = std::max(end, instr.Id());
                    } else {
                        auto& end = use_end[value.Id()];
                        end = std::max(end, instr.Id());
                    }
                }
            }
        }
        for (auto& instr : lir_block->GetInstList()) {
            if (instr.HasValue()) {
                auto start = instr.Id();
                auto end = use_end[start];
                if (!end) {
                    continue;
                }
                live_interval.push_back({&instr, start, end});
            }
        }
    }

    void ExpireOldIntervals(LiveInterval& current) {
        for (auto it = active_lives.begin(); it != active_lives.end();) {
            if (it->end < current.start) {
                auto value_type = reg_alloc->ValueType(ir::Value(it->inst));
                if (value_type == backend::RegAlloc::GPR) {
                    FreeGPR(reg_alloc->ValueGPR(it->inst->Id()).id);
                } else if (value_type == backend::RegAlloc::FPR) {
                    FreeFPR(reg_alloc->ValueFPR(it->inst->Id()).id);
                } else {
                    auto slot = reg_alloc->ValueMem(it->inst->Id()).offset;
                    FreeSpill(slot);
                    if (IsFloatValue(it->inst)) {
                        FreeSpill(slot + 1);
                    }
                }
                it = active_lives.erase(it);  // Remove expired intervals
            } else {
                ++it;
            }
        }
    }

    void GrowSpillStack(u32 new_item_size) {
        spill_slot_cursor = spill_slots.size();
        spill_slots.resize(spill_slot_cursor + new_item_size);
    }

    static bool IsFloatValue(Inst* inst) {
        auto value_type = inst->ReturnType();
        return value_type >= ValueType::V8 && value_type <= ValueType::V256;
    }

    int AllocGPR() {
        if (auto alloc = active_gprs.GetFirstClear(); alloc >= 0) {
            active_gprs.Mark(alloc);
            return alloc;
        }
        return -1;
    }

    int AllocFPR() {
        if (auto alloc = active_fprs.GetFirstClear(); alloc >= 0) {
            active_fprs.Mark(alloc);
            return alloc;
        }
        return -1;
    }

    void SpillAtInterval(LiveInterval& interval) {
        auto is_float = IsFloatValue(interval.inst);
        auto slot_size = is_float ? 2 : 1;
        if (is_float) {
            s32 slot{-1};
            for (int i = 0; i + 1 < spill_slots.size(); i += 2) {
                if (!spill_slots[i] && !spill_slots[i + 1]) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                slot = spill_slots.size();
                GrowSpillStack(slot_size);
            }
            reg_alloc->MapMemSpill(interval.inst->Id(), ir::SpillSlot{static_cast<u16>(slot)});
            spill_slots[slot] = true;
        } else {
            auto itr = std::find(spill_slots.begin(), spill_slots.end(), false);
            if (itr != spill_slots.end()) {
                u16 slot = std::distance(spill_slots.begin(), itr);
                reg_alloc->MapMemSpill(interval.inst->Id(), ir::SpillSlot{slot});
                spill_slots[slot] = true;
            } else {
                // grow stack
                u16 slot = spill_slot_cursor;
                reg_alloc->MapMemSpill(interval.inst->Id(), ir::SpillSlot{slot});
                GrowSpillStack(slot_size);
                spill_slots[slot] = true;
            }
        }
    }

    void FreeGPR(u32 id) {
        ASSERT(active_gprs.Get(id));
        active_gprs.Clear(id);
    }

    void FreeFPR(u32 id) {
        ASSERT(active_fprs.Get(id));
        active_fprs.Clear(id);
    }

    void FreeSpill(u32 slot) {
        ASSERT(spill_slots[slot]);
        spill_slots[slot] = false;
    }

    HIRFunction* function;
    Block* block;
    backend::RegAlloc* reg_alloc;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    backend::GPRSMask active_gprs;
    backend::FPRSMask active_fprs;
    Vector<bool> spill_slots{};
    u16 spill_slot_cursor{0};
};

class VRegisterAllocator {
public:
    explicit VRegisterAllocator(Block* block)
            : block(block), live_interval(), active_lives() {
    }

    void AllocateRegisters() {
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

    void ExpireOldIntervals(LiveInterval& current) {
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

    bool IsFloatValue(Inst* inst) {
        auto value_type = inst->ReturnType();
        return value_type >= ValueType::V8 && value_type <= ValueType::V256;
    }

    void GrowVRegs(u32 new_item_size) {
        active_v_regs_cursor = active_v_regs.size();
        active_v_regs.resize(active_v_regs_cursor + new_item_size);
    }

    void AllocVReg(LiveInterval& interval) {
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

private:

    void CollectLiveIntervals() {
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

    Block* block;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    Vector<bool> active_v_regs{};
    u16 active_v_regs_cursor{0};
};

void RegisterAllocPass::Run(HIRBuilder* hir_builder, backend::RegAlloc* reg_alloc) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func, reg_alloc);
    }
}

void RegisterAllocPass::Run(HIRFunction* hir_function, backend::RegAlloc* reg_alloc) {
    LinearScanAllocator linear_scan{hir_function, reg_alloc};
    linear_scan.AllocateRegisters();
}

void RegisterAllocPass::Run(ir::Block* block, backend::RegAlloc* reg_alloc) {
    LinearScanAllocator allocator{block, reg_alloc};
    allocator.AllocateRegisters();
}

void VRegisterAllocPass::Run(ir::Block* block) {
    VRegisterAllocator allocator{block};
    allocator.AllocateRegisters();
}

}  // namespace swift::runtime::ir
