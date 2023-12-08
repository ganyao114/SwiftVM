//
// Created by 甘尧 on 2023/12/6.
//

#include "register_alloc_pass.h"

namespace swift::runtime::ir {

struct LiveInterval {
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
            : function(function), block(), reg_alloc(alloc), live_interval{function->MaxInstrCount()}, active_lives() {}

    explicit LinearScanAllocator(Block* block, backend::RegAlloc* alloc)
            : function(), block(block), reg_alloc(alloc), live_interval{block->GetInstList().size()}, active_lives() {}

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

//            if (active_lives.size() == static_cast<size_t>(numAvailableRegisters)) {
//                SpillAtInterval(interval);
//            } else {
//                int reg = AllocateRegister();
//                interval.registerAssigned = reg;
//                active_lives.push_back(interval);
//                AssignRegisterToVariable(interval.variable, reg);
//            }
        }
    }

private:

    void CollectLiveIntervals(HIRFunction* hir_function) {
        for (auto& hir_value : hir_function->GetHIRValues()) {
            u32 end{hir_value.GetOrderId()};
            std::for_each(hir_value.uses.begin(), hir_value.uses.end(), [&end](auto& use) {
                end = std::max(end, (u32) use.inst->Id());
            });
            live_interval[hir_value.GetOrderId()] = {hir_value.GetOrderId(), end};
        }
    }

    void CollectLiveIntervals(Block *lir_block) {

    }

    void ExpireOldIntervals(LiveInterval &current) {
        for (auto it = active_lives.begin(); it != active_lives.end();) {
            if (it->end < current.start) {
//                FreeRegister(it->registerAssigned);
                it = active_lives.erase(it); // Remove expired intervals
            } else {
                ++it;
            }
        }
    }

    HIRFunction* function;
    Block *block;
    backend::RegAlloc* reg_alloc;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    u16 active_general_value{};
    u16 active_float_value{};
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

}  // namespace swift::runtime::ir
