#include "aarch64/operands-aarch64.h"
#include "lower_atomics.h"

#include <algorithm>
#include <map>
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {
AtomicActions LowerAtomicFallbacks(std::span<const ir::Block* const> blocks,
                                   compile::AllocationView allocation,
                                   bool lse,
                                   bool biased) {
    AtomicActions plan;
    struct Candidate {
        u16 instruction;
        UnalignedAtomicFallbackKey key;
    };
    std::vector<Candidate> candidates;
    std::map<UnalignedAtomicFallbackKey, u32> counts;
    if (!lse) return plan;
    const auto mapped = [&](ir::Value value) -> std::optional<u8> {
        if (allocation.ValueType(value) != compile::Allocation::GPR) return std::nullopt;
        return static_cast<u8>(allocation.ValueGPR(value).id);
    };
    for (auto* block : blocks)
        for (auto& inst : block->GetInstList()) {
            UnalignedAtomicFallbackKey key{};
            ir::Value address{}, first{}, second{};
            switch (inst.GetOp()) {
                case ir::OpCode::CompareAndSwap:
                    key.kind = UnalignedAtomicFallbackKind::CompareAndSwap;
                    address = inst.GetArg<ir::Value>(0);
                    first = inst.GetArg<ir::Value>(1);
                    second = inst.GetArg<ir::Value>(2);
                    break;
                case ir::OpCode::AtomicExchange:
                    key.kind = UnalignedAtomicFallbackKind::Exchange;
                    address = inst.GetArg<ir::Value>(0);
                    first = inst.GetArg<ir::Value>(1);
                    break;
                case ir::OpCode::AtomicFetchAdd:
                    key.kind = UnalignedAtomicFallbackKind::FetchAdd;
                    address = inst.GetArg<ir::Value>(0);
                    first = inst.GetArg<ir::Value>(1);
                    break;
                default:
                    continue;
            }
            key.type = first.Type();
            const u32 width = ir::GetValueSizeByte(key.type);
            if (width != sizeof(u32) && width != sizeof(u64)) continue;
            const auto result = mapped(ir::Value{&inst}), first_code = mapped(first);
            const auto address_code = biased ? std::optional<u8>{static_cast<u8>(
                                                       vixl::aarch64::mem_scratch.GetCode())}
                                             : mapped(address);
            if (!result || !first_code || !address_code || *result == *first_code) continue;
            key.address = *address_code;
            key.result = *result;
            key.first = *first_code;
            if (key.kind == UnalignedAtomicFallbackKind::CompareAndSwap) {
                const auto second_code = mapped(second);
                if (!second_code || *result == *second_code) continue;
                key.second = *second_code;
            }
            ASSERT(inst.Id() < allocation.MapCount());
            candidates.push_back({inst.Id(), key});
            ++counts[key];
        }
    ASSERT(counts.size() < UINT16_MAX);
    for (const auto& [key, count] : counts) plan.cohorts.push_back({key, count});
    for (const auto& candidate : candidates) {
        const auto cohort = std::lower_bound(
                plan.cohorts.begin(),
                plan.cohorts.end(),
                candidate.key,
                [](const AtomicCohort& row, const auto& key) { return row.key < key; });
        ASSERT(cohort != plan.cohorts.end() && cohort->key == candidate.key);
        plan.instructions.push_back(
                {candidate.instruction, static_cast<u16>(cohort - plan.cohorts.begin())});
    }
    std::sort(plan.instructions.begin(), plan.instructions.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(plan.instructions.begin(),
                              plan.instructions.end(),
                              [](const auto& a, const auto& b) {
                                  return a.instruction == b.instruction;
                              }) == plan.instructions.end());
    return plan;
}
}  // namespace swift::runtime::backend::arm64
