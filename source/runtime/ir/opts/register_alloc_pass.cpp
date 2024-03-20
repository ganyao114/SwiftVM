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
            u32 end{hir_value.GetOrderId()};
            std::for_each(hir_value.uses.begin(), hir_value.uses.end(), [&end](auto& use) {
                end = std::max(end, (u32)use.inst->Id());
            });
            live_interval.push_back({hir_value.value.Def(), hir_value.GetOrderId(), end});
        }
    }

    void CollectLiveIntervals(Block* lir_block) {
        ASSERT_MSG(lir_block, "block == null");
        StackVector<u16, 32> use_end{};
        use_end.resize(lir_block->GetInstList().size());
        for (auto& instr : lir_block->GetInstList()) {
            auto values = instr.GetValues();
            for (auto &value : values) {
                auto &end = use_end[value.Id()];
                end = std::max(end, instr.Id());
            }
        }
        for (auto& instr : lir_block->GetInstList()) {
            auto end = use_end[instr.Id()];
            live_interval.push_back({&instr, instr.Id(), end});
        }
    }

    void ExpireOldIntervals(LiveInterval& current) {
        for (auto it = active_lives.begin(); it != active_lives.end();) {
            if (it->end < current.start) {
                if (!IsFloatValue(it->inst)) {
                    FreeGPR(reg_alloc->ValueGPR(it->inst->Id()).id);
                } else {
                    FreeFPR(reg_alloc->ValueFPR(it->inst->Id()).id);
                }
                it = active_lives.erase(it);  // Remove expired intervals
            } else {
                ++it;
            }
        }
    }

    void SpillAtInterval(LiveInterval& interval) {
        auto is_float = IsFloatValue(interval.inst);

    }

    bool IsFloatValue(Inst* inst) {
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

    void FreeGPR(u32 id) {
        ASSERT(active_gprs.Get(id));
        active_gprs.Clear(id);
    }

    void FreeFPR(u32 id) {
        ASSERT(active_fprs.Get(id));
        active_fprs.Clear(id);
    }

    HIRFunction* function;
    Block* block;
    backend::RegAlloc* reg_alloc;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    backend::GPRSMask active_gprs;
    backend::FPRSMask active_fprs;
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

//            if (!IsFloatValue(interval.inst)) {
//                if (auto alloc = AllocGPR(); alloc >= 0) {
//                    active_lives.push_back(interval);
//                    reg_alloc->MapRegister(interval.inst->Id(), HostGPR{(u16)alloc});
//                } else {
//                    SpillAtInterval(interval);
//                }
//            } else {
//                if (auto alloc = AllocFPR(); alloc >= 0) {
//                    active_lives.push_back(interval);
//                    reg_alloc->MapRegister(interval.inst->Id(), HostFPR{(u16)alloc});
//                } else {
//                    SpillAtInterval(interval);
//                }
//            }
//            reg_alloc->SetActiveRegs(interval.inst->Id(), active_fprs, active_gprs);
        }
    }

    void ExpireOldIntervals(LiveInterval& current) {
        for (auto it = active_lives.begin(); it != active_lives.end();) {
            if (it->end < current.start) {
                if (!IsFloatValue(it->inst)) {
                    FreeGPR(reg_alloc->ValueGPR(it->inst->Id()).id);
                } else {
                    FreeFPR(reg_alloc->ValueFPR(it->inst->Id()).id);
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

private:

    void CollectLiveIntervals() {
        StackVector<u16, 32> use_end{};
        use_end.resize(block->GetInstList().size());
        for (auto& instr : block->GetInstList()) {
            auto values = instr.GetValues();
            for (auto &value : values) {
                auto &end = use_end[value.Id()];
                end = std::max(end, instr.Id());
            }
        }
        for (auto& instr : block->GetInstList()) {
            auto end = use_end[instr.Id()];
            live_interval.push_back({&instr, instr.Id(), end});
        }
    }

    Block* block;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
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

void VRegisterAllocPass::Run(ir::Block* block) {

}

}  // namespace swift::runtime::ir
