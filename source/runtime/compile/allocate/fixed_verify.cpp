#include "fixed_validation.h"

#include <algorithm>
#include <bit>
#include "allocation.h"
#include "internal.h"
#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::compile {
namespace {
// Preserve the complete u32 instruction/value number. Only queried homes
// enter the writer buffer; target consumption keys remain complete.
u64 HomeKey(u16 home, u32 instruction) { return (static_cast<u64>(home) << 32) | instruction; }
}  // namespace
static bool FixedConstraintsHold(const ir::Block* block,
                                 const AllocationView& allocation,
                                 const FixedValidationInputs& input,
                                 std::span<const PhysicalGPRWrite> target_writes,
                                 bool assert_on_fail,
                                 u32* fail_kind = nullptr) {
    u32 constrained_homes = 0;
    for (const auto& inst : block->GetInstList()) {
        if (const auto* fixed = allocation.FixedGPRDefinitionAt(inst.Id())) {
            ASSERT(fixed->home < 32);
            constrained_homes |= 1u << fixed->home;
            if (fixed->transferred_home != UINT16_MAX) {
                ASSERT(fixed->transferred_home < 32);
                constrained_homes |= 1u << fixed->transferred_home;
            }
        }
    }
    if (!constrained_homes) return true;
    bool ok = true;
    std::vector<u64> writers;
    std::vector<const PhysicalGPRWrite*> consumed_inputs;
    const auto* range = input.BlockFor(block);
    ASSERT(range && range->index);
    const auto find_definition = [&](u32 id) { return range->index->FindInstruction(id); };
    for (const auto* inst : range->index->Instructions()) {
        const auto* rule = input.SiteFor(inst->Id());
        const u8 flags = rule ? rule->flags : 0;
        u32 mask = rule ? rule->clobbers : 0;
        if ((flags & FixedValidationInputs::HostWrite) &&
            !allocation.IsHostWriteCoalesced(inst->Id()))
            mask |= 1u << rule->home;
        if (inst->HasValue() && !inst->IsBitCastOperation() &&
            !(allocation.IsWidthChainCoalesced(inst->Id()) &&
              (flags & FixedValidationInputs::WidthAlias)) &&
            !allocation.IsFixedGPRDefinitionElided(inst->Id()) &&
            allocation.ValueType(ir::Value{inst}) == AllocationView::GPR) {
            const auto home = allocation.ValueGPR(ir::Value{inst}).id;
            const bool read_alias = (flags & FixedValidationInputs::HostRead) &&
                                    rule->home == home &&
                                    ((flags & FixedValidationInputs::FullRead) ||
                                     allocation.IsHostReadCoalesced(inst->Id()));
            if (!read_alias) mask |= 1u << home;
        }
        mask &= constrained_homes;
        while (mask) {
            writers.push_back(HomeKey(std::countr_zero(mask), inst->Id()));
            mask &= mask - 1;
        }
    }
    for (const auto& write : target_writes) {
        ASSERT(write.home < 31);
        if (constrained_homes & (1u << write.home))
            writers.push_back(HomeKey(write.home, write.instruction));
        if (write.consumed_value != UINT32_MAX) {
            ASSERT(write.instruction < write.consuming_use);
            ASSERT(find_definition(write.instruction) && find_definition(write.consuming_use));
            consumed_inputs.push_back(&write);
        }
    }
    std::sort(writers.begin(), writers.end());
    std::sort(consumed_inputs.begin(), consumed_inputs.end(), [](const auto* a, const auto* b) {
        return HomeKey(a->home, a->consumed_value) < HomeKey(b->home, b->consumed_value);
    });
    for (size_t i = 1; i < consumed_inputs.size(); ++i)
        ASSERT(HomeKey(consumed_inputs[i - 1]->home, consumed_inputs[i - 1]->consumed_value) !=
               HomeKey(consumed_inputs[i]->home, consumed_inputs[i]->consumed_value));
    auto check = [&](u32 value, u16 home, u32 after, u32 before) {
        if (!consumed_inputs.empty()) {
            const auto key = HomeKey(home, value);
            const auto found =
                    std::lower_bound(consumed_inputs.begin(),
                                     consumed_inputs.end(),
                                     key,
                                     [](const PhysicalGPRWrite* write, u64 key) {
                                         return HomeKey(write->home, write->consumed_value) < key;
                                     });
            if (found != consumed_inputs.end() &&
                HomeKey((*found)->home, (*found)->consumed_value) == key &&
                before == (*found)->consuming_use)
                before = (*found)->instruction;
        }
        auto next = std::upper_bound(writers.begin(), writers.end(), HomeKey(home, after));
        const auto limit = HomeKey(home, before);
        const auto* residence = allocation.FixedGPRDefinitionAt(value);
        for (; next != writers.end() && *next < limit; ++next) {
            bool transparent = false;
            // A full-width residence whose value is a 32-bit-fitting
            // constant survives an extension-chain rewrite of its home:
            // the chain reproduces exactly the same bits.  This mirrors
            // the LoadImm case in the W-write predicates.
            const auto* value_rule = input.SiteFor(value);
            const bool narrow_constant =
                    value_rule && (value_rule->flags & FixedValidationInputs::NarrowConstant);
            const auto* writer = find_definition(static_cast<u32>(*next));
            if (residence && (residence->width <= 4 || narrow_constant) && writer) {
                const auto* step = input.StepFor(writer);
                const auto* source = step ? step->source : nullptr;
                auto source_type = step ? step->source_type : ir::ValueType{};
                while (source && source->Id() != value) {
                    if (source->IsBitCastOperation()) {
                        const auto parent = source->GetArg<ir::Value>(0);
                        source = parent.Def();
                        source_type = parent.Defined() ? parent.Type() : ir::ValueType{};
                    } else if (const auto* parent = input.StepFor(source);
                               parent && parent->parent) {
                        source = parent->source;
                        source_type = parent->source_type;
                    } else
                        break;
                }
                transparent = source && source->Id() == value &&
                              ir::GetValueSizeByte(source_type) >= residence->width;
            }
            if (!transparent) break;
        }
        const bool clear = next == writers.end() || *next >= limit;
        if (assert_on_fail) {
            const auto next_instruction = next == writers.end() || (*next >> 32) != home
                                                  ? UINT32_MAX
                                                  : static_cast<u32>(*next);
            ASSERT_MSG(clear,
                       "fixed GPR value {} ({}, width {}) in x{} is overwritten by IR {} ({}) "
                       "before use {}",
                       value,
                       find_definition(value) ? find_definition(value)->GetOp() : ir::OpCode::Void,
                       residence ? residence->width : 0,
                       home,
                       next_instruction,
                       next_instruction != UINT32_MAX && find_definition(next_instruction)
                               ? find_definition(next_instruction)->GetOp()
                               : ir::OpCode::Void,
                       before);
        } else {
            if (!clear && fail_kind && !*fail_kind) *fail_kind = 2;
            ok &= clear;
        }
    };
    for (auto& inst : block->GetInstList()) {
        const auto* fixed = allocation.FixedGPRDefinitionAt(inst.Id());
        if (!fixed) continue;
        if (fixed->preserved) {
            const bool shape = fixed->end == fixed->transfer &&
                               fixed->transfer <= fixed->last_use && fixed->width <= sizeof(u64) &&
                               fixed->width * 8 + fixed->bit_offset <= 64;
            if (assert_on_fail)
                ASSERT(shape);
            else {
                if (!shape && fail_kind && !*fail_kind) *fail_kind = 3;
                ok &= shape;
            }
            const auto kind = allocation.ValueType(ir::Value{&inst});
            const bool kind_ok = kind == AllocationView::GPR || kind == AllocationView::MEM;
            if (assert_on_fail)
                ASSERT(kind_ok);
            else {
                if (!kind_ok && fail_kind && !*fail_kind) *fail_kind = 3;
                ok &= kind_ok;
            }
            if (kind == AllocationView::GPR) {
                const bool displaced = allocation.ValueGPR(ir::Value{&inst}).id != fixed->home;
                if (assert_on_fail)
                    ASSERT(displaced);
                else {
                    if (!displaced && fail_kind && !*fail_kind) *fail_kind = 3;
                    ok &= displaced;
                }
            }
        } else {
            const auto kind = allocation.ValueType(ir::Value{&inst});
            const bool placed = kind == AllocationView::GPR &&
                                allocation.ValueGPR(ir::Value{&inst}).id == fixed->home;
            if (assert_on_fail) {
                ASSERT(kind == AllocationView::GPR);
                ASSERT(allocation.ValueGPR(ir::Value{&inst}).id == fixed->home);
            } else {
                if (!placed && fail_kind && !*fail_kind) *fail_kind = 1;
                ok &= placed;
            }
        }
        check(inst.Id(), fixed->home, inst.Id(), fixed->end);
        if (fixed->transferred_home != UINT16_MAX)
            check(inst.Id(), fixed->transferred_home, fixed->transfer, fixed->last_use);
    }
    return ok;
}

void VerifyFixedConstraints(const ir::Block* block,
                            const AllocationView& allocation,
                            const FixedValidationInputs& input,
                            std::span<const PhysicalGPRWrite> target_writes) {
    (void)FixedConstraintsHold(block, allocation, input, target_writes, true);
}

bool CheckFixedConstraints(const ir::Block* block,
                           const AllocationView& allocation,
                           const FixedValidationInputs& input,
                           std::span<const PhysicalGPRWrite> target_writes,
                           u32* fail_kind) {
    return FixedConstraintsHold(block, allocation, input, target_writes, false, fail_kind);
}

}  // namespace swift::runtime::compile

namespace swift::runtime::compile::detail {
void LinearScan::VerifyFixedConstraints() {
    const auto verify = [&](const ir::Block* member) {
        if (!inputs.fixed_validation) {
            for (const auto& inst : member->GetInstList())
                ASSERT(!reg_alloc->HasFixedGPRDefinition(inst.Id()));
            return;
        }
        compile::VerifyFixedConstraints(member, *reg_alloc, *inputs.fixed_validation);
    };
    if (function)
        for (const auto& member : function->GetHIRBlocksRPO()) verify(member.GetBlock());
    else
        verify(block);
}
}  // namespace swift::runtime::compile::detail
