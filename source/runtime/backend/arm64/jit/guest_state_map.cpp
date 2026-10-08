#include "guest_state_map.h"

#include <algorithm>
#include <bit>
#include "runtime/backend/reg_alloc.h"
#include "runtime/backend/arm64/helper_call_contract.h"

namespace swift::runtime::backend::arm64 {

void GuestStateMap::Analyze(ir::Block* next_block,
                            const FeatureSet& next_features) {
    block = next_block;
    features = next_features;
    fixed_home_clobbers.reset();
    successful_survival_scans = 0;
    block_entry_width_facts = {};
    if (const auto found = function_entry_width_facts.find(block);
        found != function_entry_width_facts.end()) {
        block_entry_width_facts = found->second;
    }
    fault_capture_values.clear();
    fault_width_captures.clear();
    fixed_home_uses.clear();
    fixed_home_use_counts.clear();
}

bool GuestStateMap::ClobbersFixedHome(const ir::Inst& inst,
                                      u32 home) const {
    if (inst.GetOp() == ir::OpCode::XchgBarrier) {
        return (::swift::runtime::backend::FixedGPRClobbers(
                        inst, features) & (1u << home)) != 0;
    }
    if (inst.GetOp() == ir::OpCode::SetHostGPR &&
        inst.GetArg<ir::Imm>(1).Get() == home) {
        return true;
    }
    return home <= 9 && HelperCallContract::InstructionClobbersGPR(inst, home,
                                                                   features);
}

GuestStateMap::ExtensionFacts GuestStateMap::CurrentEntryExtensionFacts(
        u32 home) const {
    return home < block_entry_width_facts.size()
            ? block_entry_width_facts[home]
            : ExtensionFacts{};
}

void GuestStateMap::BuildFixedHomeClobberIndex() const {
    auto index = std::make_unique<FixedHomeClobberIndex>();
    for (auto& inst : block->GetInstList()) {
        u32 mask{};
        if (inst.GetOp() == ir::OpCode::XchgBarrier) {
            mask = ::swift::runtime::backend::FixedGPRClobbers(inst, features);
        } else if (inst.GetOp() == ir::OpCode::SetHostGPR) {
            const auto home = inst.GetArg<ir::Imm>(1).Get();
            if (home < index->size()) mask = 1u << home;
        } else if (const auto helper = HelperCallContract::Resolve(inst, features)) {
            // Resolve an exact helper contract once, rather than once per
            // register and interval. Only caller-saved guest homes apply here.
            for (u32 home = 0; home <= 9; ++home)
                if (helper->ClobbersGPR(home)) mask |= 1u << home;
        }
        while (mask) {
            const auto home = std::countr_zero(mask);
            (*index)[home].push_back(inst.Id());
            mask &= mask - 1;
        }
    }
    for (auto& ids : *index) std::sort(ids.begin(), ids.end());
    fixed_home_clobbers = std::move(index);
}

bool GuestStateMap::FixedHomeSurvives(u32 home,
                                      u32 after,
                                      u32 before) const {
    ASSERT(block);
    if (before <= after || before - after <= 1) return true;
    // Wait for three complete scans before paying to resolve contracts and
    // collect clobbers for every home. Two-query transfers keep scanning.
    constexpr u8 min_indexed_scans = 3;
    if (home < 32 && successful_survival_scans >= min_indexed_scans) {
        if (!fixed_home_clobbers) BuildFixedHomeClobberIndex();
        const auto& ids = (*fixed_home_clobbers)[home];
        const auto next = std::upper_bound(ids.begin(), ids.end(), after);
        return next == ids.end() || *next >= before;
    }
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() > after && inst.Id() < before &&
            ClobbersFixedHome(inst, home)) {
            return false;
        }
    }
    // Small blocks and early rejections keep their cheap linear path.
    // Several successful whole-block traversals amortize the index build.
    const auto& instructions = block->GetInstList();
    if (!instructions.empty() && instructions.back().Id() >= instructions.front().Id() &&
        instructions.back().Id() - instructions.front().Id() >= 63 &&
        successful_survival_scans < min_indexed_scans)
        ++successful_survival_scans;
    return true;
}

bool GuestStateMap::PublicationWindowSafe(
        u32 home,
        ir::Value early_value,
        u32 after,
        u32 before,
        const ir::Inst* ignored) const {
    ASSERT(block);
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= after || inst.Id() >= before || &inst == ignored) {
            continue;
        }
        if (ClobbersFixedHome(inst, home)) {
            return false;
        }
        if (MayFaultOrObserve(inst) &&
            !FaultCaptureContains(
                    inst, home, early_value.Def(),
                    ir::GetValueSizeByte(early_value.Type()) <= sizeof(u32))) {
            return false;
        }
    }
    return true;
}

bool GuestStateMap::MayFaultOrObserve(const ir::Inst& inst) const {
    const auto helper = HelperCallContract::Resolve(inst, features);
    return helper ? helper->RequiresGuestStatePublication()
                  : MayFaultOrObserve(inst.GetOp());
}

bool GuestStateMap::MayFaultOrObserve(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::CheckMemoryAlignment:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
        case ir::OpCode::Sse42Str:
            return true;
        default:
            return false;
    }
}

}  // namespace swift::runtime::backend::arm64
