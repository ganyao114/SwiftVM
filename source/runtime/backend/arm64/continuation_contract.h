#pragma once

#include <cstdint>

namespace vixl::aarch64 {
class MacroAssembler;
class Register;
}  // namespace vixl::aarch64

namespace swift::runtime::backend::arm64 {

enum class ContinuationTraversal : std::uintptr_t {
    Branch = 0,
    Call = 1,
    CallMiss = 3,
};

class ContinuationContract final {
public:
    static constexpr unsigned kCallTraversalBit = 0;
    static constexpr unsigned kPublishFrameBit = 1;
    static constexpr std::uintptr_t kTraversalTagMask = 3;

    [[nodiscard]] static void* EncodeTraversal(void* target, ContinuationTraversal traversal);
    static void PublishFrame(vixl::aarch64::MacroAssembler& masm);
    static void PublishExternalFrame(vixl::aarch64::MacroAssembler& masm);
    static void PublishExternalFrame(vixl::aarch64::MacroAssembler& masm,
                                     const vixl::aarch64::Register& guest_return);
    static void ConsumeFrame(vixl::aarch64::MacroAssembler& masm,
                             const vixl::aarch64::Register& guest_return,
                             const vixl::aarch64::Register& host_continuation);
};

}  // namespace swift::runtime::backend::arm64
