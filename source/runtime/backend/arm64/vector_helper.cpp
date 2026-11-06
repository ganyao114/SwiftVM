#include "vector_helper.h"

#include <cstdlib>

namespace swift::runtime::backend::arm64 {
namespace {
#if defined(__aarch64__)
// This is the existing fixed-home ABI save set, plus the exact incoming NZCV.
// v0/v1 and v8/v9 (pin-on) or v30/v31 (pin-off) are writer temporaries.
// Saving the superset keeps this same bridge valid for either physical bank.
__attribute__((naked)) void CallVectorHelper() {
    __asm__ volatile(
        "sub sp,sp,#592\n"
        "stp x1,x2,[sp,#0]\n" "stp x3,x4,[sp,#16]\n"
        "stp x5,x6,[sp,#32]\n" "stp x7,x8,[sp,#48]\n"
        "stp x9,x10,[sp,#64]\n" "stp x11,x12,[sp,#80]\n"
        "stp x13,x14,[sp,#96]\n" "stp x15,x30,[sp,#112]\n"
        "stp q2,q3,[sp,#128]\n" "stp q4,q5,[sp,#160]\n"
        "stp q6,q7,[sp,#192]\n" "stp q16,q17,[sp,#224]\n"
        "stp q18,q19,[sp,#256]\n" "stp q20,q21,[sp,#288]\n"
        "stp q22,q23,[sp,#320]\n" "stp q24,q25,[sp,#352]\n"
        "stp q26,q27,[sp,#384]\n" "stp q28,q29,[sp,#416]\n"
        "stp q30,q31,[sp,#448]\n" "stp q10,q11,[sp,#480]\n"
        "stp q12,q13,[sp,#512]\n" "stp q14,q15,[sp,#544]\n"
        "mrs x3,nzcv\n" "str x3,[sp,#576]\n"
        "mov x30,x0\n" "mov x0,x16\n" "mov x1,x17\n" "blr x30\n"
        "fmov d0,x0\n" "ins v0.d[1],x1\n"
        "ldr x3,[sp,#576]\n" "msr nzcv,x3\n"
        "ldp q2,q3,[sp,#128]\n" "ldp q4,q5,[sp,#160]\n"
        "ldp q6,q7,[sp,#192]\n" "ldp q16,q17,[sp,#224]\n"
        "ldp q18,q19,[sp,#256]\n" "ldp q20,q21,[sp,#288]\n"
        "ldp q22,q23,[sp,#320]\n" "ldp q24,q25,[sp,#352]\n"
        "ldp q26,q27,[sp,#384]\n" "ldp q28,q29,[sp,#416]\n"
        "ldp q30,q31,[sp,#448]\n" "ldp q10,q11,[sp,#480]\n"
        "ldp q12,q13,[sp,#512]\n" "ldp q14,q15,[sp,#544]\n"
        "ldp x1,x2,[sp,#0]\n" "ldp x3,x4,[sp,#16]\n"
        "ldp x5,x6,[sp,#32]\n" "ldp x7,x8,[sp,#48]\n"
        "ldp x9,x10,[sp,#64]\n" "ldp x11,x12,[sp,#80]\n"
        "ldp x13,x14,[sp,#96]\n" "ldp x15,x30,[sp,#112]\n"
        "add sp,sp,#592\n" "ret\n");
}
#else
void CallVectorHelper() { std::abort(); } // Cross-host code generation only.
#endif
} // namespace

std::uintptr_t VectorHelperBridge() noexcept {
    return reinterpret_cast<std::uintptr_t>(&CallVectorHelper);
}
} // namespace swift::runtime::backend::arm64
