#include "runtime/backend/arm64/continuation_contract.h"

#include "aarch64/macro-assembler-aarch64.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

void* ContinuationContract::EncodeTraversal(void* target, ContinuationTraversal traversal) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    VIXL_ASSERT((address & kTraversalTagMask) == 0);
    return reinterpret_cast<void*>(address | static_cast<std::uintptr_t>(traversal));
}

void ContinuationContract::PublishFrame(MacroAssembler& masm) {
    masm.Stp(x14, x30, MemOperand(x25, -16, PreIndex));
}

void ContinuationContract::PublishExternalFrame(MacroAssembler& masm) {
    PublishExternalFrame(masm, x14);
}

void ContinuationContract::PublishExternalFrame(MacroAssembler& masm,
                                                const Register& guest_return) {
    masm.Stp(guest_return, xzr, MemOperand(x25, -16, PreIndex));
}

void ContinuationContract::ConsumeFrame(MacroAssembler& masm,
                                        const Register& guest_return,
                                        const Register& host_continuation) {
    masm.Ldp(guest_return, host_continuation, MemOperand(x25, 16, PostIndex));
}

}  // namespace swift::runtime::backend::arm64
