#include "translator.h"

#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

bool JitTranslator::CanUseLSE() const {
    return True(context.GetConfig().arm64_features & Arm64Features::Atomics);
}

void JitTranslator::EmitLSECompareAndSwap(ir::ValueType type,
                                          const Register& result,
                                          const Register& expected,
                                          const Register& desired,
                                          const Register& address) {
    if (result.GetCode() != expected.GetCode()) {
        __ Mov(result, expected);
    }
    vixl::CPUFeaturesScope atomics(&masm, vixl::CPUFeatures::kAtomics);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Casalb(result.W(), desired.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Casalh(result.W(), desired.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Casal(result.W(), desired.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Casal(result, desired, MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
}

void JitTranslator::EmitLSEExchange(ir::ValueType type,
                                    const Register& result,
                                    const Register& desired,
                                    const Register& address) {
    vixl::CPUFeaturesScope atomics(&masm, vixl::CPUFeatures::kAtomics);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Swpalb(desired.W(), result.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Swpalh(desired.W(), result.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Swpal(desired.W(), result.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Swpal(desired, result, MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
}

void JitTranslator::EmitLSEFetchAdd(ir::ValueType type,
                                    const Register& result,
                                    const Register& addend,
                                    const Register& address) {
    vixl::CPUFeaturesScope atomics(&masm, vixl::CPUFeatures::kAtomics);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaddalb(addend.W(), result.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaddalh(addend.W(), result.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaddal(addend.W(), result.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaddal(addend, result, MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
}

#undef __

}  // namespace swift::runtime::backend::arm64
