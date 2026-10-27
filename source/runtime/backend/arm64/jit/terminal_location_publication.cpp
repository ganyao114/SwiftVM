#include "terminal_location_publication.h"

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

void TerminalLocationPublication::Reset() {
    for (auto& label : miss_labels) {
        label.reset();
    }
}

Label* TerminalLocationPublication::GetOrCreateMissLabel(
        const XRegister& target) {
    auto& label = miss_labels[target.GetCode()];
    if (!label) {
        label = std::make_unique<Label>();
    }
    return label.get();
}

Label* TerminalLocationPublication::MissLabel(const XRegister& target) const {
    return miss_labels[target.GetCode()].get();
}

TerminalLocationPublication::RecoveryOffsets
TerminalLocationPublication::EmitColdPaths(JitContext& context) {
    auto& masm = context.GetMasm();
    RecoveryOffsets offsets{};
    for (u32 reg = 0; reg < miss_labels.size(); ++reg) {
        if (!miss_labels[reg]) {
            continue;
        }
        offsets[reg] = masm.GetBuffer()->GetSizeInBytes();
        masm.Bind(miss_labels[reg].get());
        masm.Str(XRegister(reg), MemOperand(state, state_offset_current_loc));
        context.ReturnHost();
    }
    Reset();
    return offsets;
}

}  // namespace swift::runtime::backend::arm64
