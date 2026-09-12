#include "translator.h"

namespace swift::runtime::backend::arm64 {

std::optional<ir::Value>
JitTranslator::MatchNarrowFlagsInput(ir::Inst* extract) {
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract ||
        extract->GetUses(false) != 1 || !cur_block) {
        return std::nullopt;
    }

    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if ((width != sizeof(u8) && width != sizeof(u16)) ||
        extract->GetArg<ir::Imm>(1).Get() != 0 ||
        extract->GetArg<ir::Imm>(2).Get() != width * 8) {
        return std::nullopt;
    }

    auto& instructions = cur_block->GetInstList();
    auto consumer = std::next(instructions.iterator_to(*extract));
    if (consumer == instructions.end() ||
        (consumer->GetOp() != ir::OpCode::Add &&
         consumer->GetOp() != ir::OpCode::Sub &&
         consumer->GetOp() != ir::OpCode::Neg) ||
        consumer->GetArg<ir::Value>(0).Def() != extract ||
        ir::GetValueSizeByte(consumer->ReturnType()) != width ||
        !True(GetPseudoFlags(&*consumer).set & ir::Flags::NZCV)) {
        return std::nullopt;
    }

    return extract->GetArg<ir::Value>(0);
}

void JitTranslator::PrepareNarrowFlagsInputs(ir::Block* block) {
    flag_state.narrow_flags_inputs.clear();
    for (auto& inst : block->GetInstList()) {
        if (auto source = MatchNarrowFlagsInput(&inst)) {
            flag_state.narrow_flags_inputs.emplace(&inst, *source);
        }
    }
}

ir::Value JitTranslator::ResolveNarrowFlagsInput(ir::Value value,
                                                 ir::Inst* consumer) {
    if (!value.Def()) {
        return value;
    }
    if (pinned_gprs.fused_pin_gpr_reads.contains(value.Def())) {
        return value;
    }
    auto candidate = flag_state.narrow_flags_inputs.find(value.Def());
    if (candidate == flag_state.narrow_flags_inputs.end()) {
        return value;
    }
    auto source = MatchNarrowFlagsInput(value.Def());
    ASSERT_MSG(source && *source == candidate->second,
               "narrow flags input proof diverged at IR {}", consumer->Id());
    ASSERT_MSG(std::next(cur_block->GetInstList().iterator_to(*value.Def())) ==
                       cur_block->GetInstList().iterator_to(*consumer),
               "narrow flags input consumer diverged at IR {}", consumer->Id());
    return candidate->second;
}

}  // namespace swift::runtime::backend::arm64
