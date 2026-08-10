#pragma once

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

class VRegisterAllocator {
public:
    explicit VRegisterAllocator(Block* block);

    void AllocateRegisters();
    void ExpireOldIntervals(LiveInterval& current);
    bool IsFloatValue(Inst* inst);
    void GrowVRegs(u32 new_item_size);
    void AllocVReg(LiveInterval& interval);

private:
    void CollectLiveIntervals();

    Block* block;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    Vector<bool> active_v_regs{};
    u16 active_v_regs_cursor{0};
};

}  // namespace swift::runtime::ir
