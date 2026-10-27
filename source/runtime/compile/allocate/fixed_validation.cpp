#include "fixed_validation.h"

#include <algorithm>
#include <functional>

namespace swift::runtime::compile {
const FixedValidationInputs::Site* FixedValidationInputs::SiteFor(u32 instruction) const {
    const auto found =
            std::lower_bound(sites.begin(), sites.end(), instruction, [](const Site& row, u32 id) {
                return row.instruction < id;
            });
    return found != sites.end() && found->instruction == instruction ? &*found : nullptr;
}
const FixedValidationInputs::Step* FixedValidationInputs::StepFor(
        const ir::Inst* instruction) const {
    const auto found = std::lower_bound(
            steps.begin(), steps.end(), instruction, [](const Step& row, const ir::Inst* inst) {
                return std::less<const ir::Inst*>{}(row.instruction, inst);
            });
    return found != steps.end() && found->instruction == instruction ? &*found : nullptr;
}
const FixedValidationInputs::BlockRange* FixedValidationInputs::BlockFor(
        const ir::Block* block) const {
    const auto found = std::lower_bound(blocks.begin(),
                                        blocks.end(),
                                        block,
                                        [](const BlockRange& row, const ir::Block* member) {
                                            return std::less<const ir::Block*>{}(row.block, member);
                                        });
    return found != blocks.end() && found->block == block ? &*found : nullptr;
}
}  // namespace swift::runtime::compile
