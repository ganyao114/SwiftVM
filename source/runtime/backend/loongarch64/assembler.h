#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/backend/loongarch64/defines.h"
#include "runtime/backend/loongarch64/host_features.h"

namespace swift::runtime::backend::loongarch64 {

class Assembler : public Xbyak_loongarch64::CodeGenerator {
public:
    // LBT is opt-in even when constructing an assembler on a LoongArch host.
    // Pass Detect() only after the runtime selects the matching lowering.
    explicit Assembler(std::size_t initial_capacity = 4096, HostFeatures features = {});

    void LI(GPR destination, std::uint64_t value);
    void MV(GPR destination, GPR source);
    void Bind(Label* label);
    void J(Label* label);
    void BEQ(GPR lhs, GPR rhs, Label* label);
    void BNE(GPR lhs, GPR rhs, Label* label);
    void BLT(GPR lhs, GPR rhs, Label* label);
    void BGE(GPR lhs, GPR rhs, Label* label);
    void BLTU(GPR lhs, GPR rhs, Label* label);
    void BGEU(GPR lhs, GPR rhs, Label* label);
    // A relocatable PC-relative address, checked within pcaddi's +/-2 MiB
    // range. The buffer can be copied to any equally aligned code-cache base.
    void LILabel(GPR destination, Label* label);
    void Finalize();
    std::size_t Size() const { return getSize(); }
    const std::uint8_t* Data() const { return getCode(); }
    const HostFeatures& Features() const { return features_; }

    // LBT ARM arithmetic updates implicit flags only: these methods have no
    // destination operand. They do not replace the ordinary integer result.
    // There is no ARMADD_D lowering; ARM64 X-register flags use software.
    void ARMADD_W(GPR lhs, GPR rhs, std::uint32_t condition);
    void ARMSUB_W(GPR lhs, GPR rhs, std::uint32_t condition);
    void ARMADC_W(GPR lhs, GPR rhs, std::uint32_t condition);
    void ARMSBC_W(GPR lhs, GPR rhs, std::uint32_t condition);
    void ARMAND_W(GPR lhs, GPR rhs, std::uint32_t condition);
    void ARMOR_W(GPR lhs, GPR rhs, std::uint32_t condition);
    void ARMXOR_W(GPR lhs, GPR rhs, std::uint32_t condition);
    void ARMMFFLAG(GPR destination, std::uint32_t mask);
    void ARMMTFLAG(GPR source, std::uint32_t mask);
    void SETARMJ(GPR destination, std::uint32_t condition);

#define SWIFT_LBT_X86_DECL(name) \
    void name##_B(GPR lhs, GPR rhs); \
    void name##_H(GPR lhs, GPR rhs); \
    void name##_W(GPR lhs, GPR rhs); \
    void name##_D(GPR lhs, GPR rhs);
    SWIFT_LBT_X86_DECL(X86ADD)
    SWIFT_LBT_X86_DECL(X86SUB)
    SWIFT_LBT_X86_DECL(X86ADC)
    SWIFT_LBT_X86_DECL(X86SBC)
    SWIFT_LBT_X86_DECL(X86AND)
    SWIFT_LBT_X86_DECL(X86OR)
    SWIFT_LBT_X86_DECL(X86XOR)
#undef SWIFT_LBT_X86_DECL
    void X86MFFLAG(GPR destination, std::uint32_t mask);
    void X86MTFLAG(GPR source, std::uint32_t mask);
    void SETX86J(GPR destination, std::uint32_t condition);

private:
    void ARMFlags(std::uint32_t opcode, GPR lhs, GPR rhs, std::uint32_t condition);
    void X86Flags(std::uint32_t opcode, GPR lhs, GPR rhs);
    void FlagTransfer(bool arm, std::uint32_t opcode, GPR reg, std::uint32_t mask);
    void SetCondition(bool arm, std::uint32_t opcode, GPR reg, std::uint32_t condition);
    void RequireLBT(bool arm) const;
    HostFeatures features_;
};

}  // namespace swift::runtime::backend::loongarch64
