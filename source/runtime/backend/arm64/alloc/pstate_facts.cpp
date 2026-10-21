#include "pstate_facts.h"

namespace swift::runtime::backend::arm64 {
namespace {
bool Clobbers(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::TestFlags: case ir::OpCode::TestNotFlags:
        case ir::OpCode::CallLambda: case ir::OpCode::CallLocation: case ir::OpCode::CallDynamic:
        case ir::OpCode::VecFCmp: case ir::OpCode::PublishFCmpFlags: case ir::OpCode::PublishSse42StrFlags:
        case ir::OpCode::X87Op: case ir::OpCode::VecShiftLeft: case ir::OpCode::VecShiftRight:
        case ir::OpCode::VecShiftRightArithmetic: return true;
        default: return false;
    }
}
}
PstateFacts BuildPstateFacts(const BlockAnalysisIndex& analysis) {
    PstateFacts facts;
    ir::Flags simple{};
    bool saved = false, clobbered = false, inverted = false, add_family = false;
    constexpr auto simple_mask = ir::Flags::Zero | ir::Flags::Negate | ir::Flags::Overflow;
    for (auto* inst : analysis.Instructions()) {
        const auto op = inst->GetOp();
        if (op == ir::OpCode::TestFlags || op == ir::OpCode::TestNotFlags)
            facts.available.emplace(inst, simple | ((inverted || add_family) ? ir::Flags::Carry : ir::Flags::None));
        if (op == ir::OpCode::AdvancePC && !facts.first_advance) facts.first_advance = inst->Id();
        if (op == ir::OpCode::SaveFlags || op == ir::OpCode::BranchOnlyFlags) {
            const auto flags = inst->GetArg<ir::Flags>(1);
            simple = flags & simple_mask;
            if (True(flags & ir::Flags::Carry)) {
                saved = true; clobbered = false; inverted = false;
                auto* producer = inst->GetArg<ir::Value>(0).Def();
                add_family = producer && (producer->GetOp() == ir::OpCode::Add || producer->GetOp() == ir::OpCode::Adc);
            }
        } else if (op == ir::OpCode::ClearFlags) {
            const auto cleared = inst->GetArg<ir::Flags>(0);
            simple &= ~cleared;
            if (True(cleared & ir::Flags::Carry)) { saved = false; inverted = false; add_family = false; clobbered = true; }
        } else if (op == ir::OpCode::SetOverflow) simple &= ~ir::Flags::Overflow;
        else if (op == ir::OpCode::InvertCarry) { inverted = saved && !clobbered; add_family = false; }
        else if (op == ir::OpCode::SetCarry) { saved = false; inverted = false; add_family = false; clobbered = true; }
        else if (Clobbers(op)) { simple = ir::Flags::None; clobbered = true; inverted = false; add_family = false; }
    }
    return facts;
}
}
