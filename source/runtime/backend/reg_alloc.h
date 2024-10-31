//
// Created by 甘尧 on 2023/10/13.
//

#pragma once

#include <bit>
#include "base/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/ir/block.h"
#include "runtime/ir/host_reg.h"

namespace swift::runtime::backend {

template<typename T = u32>
class RegisterMask {
public:

    explicit RegisterMask() : mask() {}

    explicit RegisterMask(T mask) : mask(mask) {}

    auto GetFirstMarked() {
        return std::countr_zero(mask);
    }

    auto GetFirstClear() {
        return std::countr_one(mask);
    }

    auto GetMarkedCount() {
        return std::popcount(mask);
    }

    auto GetClearCount() {
        return GetAllCount() - std::popcount(mask);
    }

    auto GetAllCount() {
        return sizeof(T) * 8;
    }

    [[nodiscard]] bool Null() const {
        return mask == 0;
    }

    [[nodiscard]] bool Get(u32 bit) const {
        return mask & (T(1) << bit);
    }

    void Mark(u32 bit) {
        mask |= (T(1) << bit);
    }

    void Clear(u32 bit) {
        mask &= ~(T(1) << bit);
    }

    void Reset(T value) {
        mask = value;
    }

private:
    T mask;
};

using GPRSMask = RegisterMask<u32>;
using FPRSMask = RegisterMask<u32>;

class RegAlloc : DeleteCopyAndMove {
public:

    explicit RegAlloc(u32 instr_size, const GPRSMask& gprs, const FPRSMask& fprs);

    enum Type : u16 {
        NONE,
        GPR,
        FPR,
        MEM,
        REF
    };

    struct Map {
        Type type{NONE};
        u16 slot{};
        GPRSMask dirty_gprs{0};
        FPRSMask dirty_fprs{0};
    };

    [[nodiscard]] const GPRSMask& GetGprs() const;
    [[nodiscard]] const FPRSMask& GetFprs() const;

    void MapRegister(u32 id, ir::HostGPR gpr);
    void MapRegister(u32 id, ir::HostFPR fpr);
    void MapMemSpill(u32 id, ir::SpillSlot slot);
    void MapReference(u32 from, u32 to);
    void SetActiveRegs(u32 id, GPRSMask &gprs, FPRSMask &fprs);

    ir::HostGPR ValueGPR(const ir::Value &value);
    ir::HostFPR ValueFPR(const ir::Value &value);
    ir::SpillSlot ValueMem(const ir::Value &value);
    ir::HostGPR ValueGPR(u32 id);
    ir::HostFPR ValueFPR(u32 id);
    ir::SpillSlot ValueMem(u32 id);
    Type ValueType(const ir::Value &value);

    [[nodiscard]] GPRSMask GetDirtyGPR() const;
    [[nodiscard]] FPRSMask GetDirtyFPR() const;

    void SetCurrent(ir::Inst *inst);

private:
    Vector<Map> alloc_result;
    u32 stack_size{};
    ir::Inst *current_ir{};
    const GPRSMask gprs;
    const FPRSMask fprs;
};

}
