#include <csignal>
#include <cstdint>
#include <cstdlib>

extern "C" std::uint64_t SvmX87Execute(void*, std::uint64_t, std::uint64_t) noexcept;

namespace {
using u64 = std::uint64_t;
using u128 = unsigned __int128;
struct DivisionResult { u64 quotient, remainder; };
[[noreturn]] void DivisionFault() {
    std::raise(SIGFPE);
    std::abort();
}
}

// The private entry stubs below adapt the writer's fixed homes to the normal
// host ABI once, rather than emitting a register-save sequence at each call.
extern "C" __attribute__((used, noinline, visibility("hidden")))
DivisionResult SvmArm64WideDivide(u64 high, u64 low, u64 divisor, unsigned config) {
    const auto bits = config & 255u;
    const bool sign = config & 256u;
    const u64 mask = bits == 64 ? ~u64{} : (u64{1} << bits) - 1;
    const u128 wide_mask = bits == 64 ? ~u128{} : (u128{1} << (bits * 2)) - 1;
    high &= mask; low &= mask; divisor &= mask;
    if (!divisor) DivisionFault();
    const u128 dividend = (u128{high} << bits) | low;
    const bool negative_dividend = sign && (high >> (bits - 1));
    const bool negative_divisor = sign && (divisor >> (bits - 1));
    const u128 magnitude = negative_dividend ? (-dividend & wide_mask) : dividend;
    const u64 denominator = negative_divisor ? (-divisor & mask) : divisor;
    const auto quotient = magnitude / denominator;
    const bool negative_quotient = negative_dividend != negative_divisor;
    const u64 limit = sign ? (u64{1} << (bits - 1)) - !negative_quotient : mask;
    if (quotient > limit) DivisionFault();
    const u64 remainder = static_cast<u64>(magnitude % denominator);
    return {(negative_quotient ? -static_cast<u64>(quotient) : static_cast<u64>(quotient)) & mask,
            (negative_dividend ? -remainder : remainder) & mask};
}

#if defined(__aarch64__)
#if defined(__APPLE__)
#define SVM_DIV_SYMBOL "_SvmArm64WideDivide"
#else
#define SVM_DIV_SYMBOL "SvmArm64WideDivide"
#endif
// x1..x15, both vector home banks and the caller's BLR return address survive.
// Callee-saves, including the linked ABI's AF input xor in x29, survive the
// ordinary C++ call. x1 retains the raw PF carrier, not a packed flags word.
// x0/x16 are the two result words; x17 remains writer scratch.
#define SVM_HELPER_SAVE_BANK \
    "sub sp,sp,#576\n" \
    "stp x1,x2,[sp,#0]\n" "stp x3,x4,[sp,#16]\n" \
    "stp x5,x6,[sp,#32]\n" "stp x7,x8,[sp,#48]\n" \
    "stp x9,x10,[sp,#64]\n" "stp x11,x12,[sp,#80]\n" \
    "stp x13,x14,[sp,#96]\n" "stp x15,x30,[sp,#112]\n" \
    "stp q2,q3,[sp,#128]\n" "stp q4,q5,[sp,#160]\n" \
    "stp q6,q7,[sp,#192]\n" "stp q16,q17,[sp,#224]\n" \
    "stp q18,q19,[sp,#256]\n" "stp q20,q21,[sp,#288]\n" \
    "stp q22,q23,[sp,#320]\n" "stp q24,q25,[sp,#352]\n" \
    "stp q26,q27,[sp,#384]\n" "stp q28,q29,[sp,#416]\n" \
    "stp q30,q31,[sp,#448]\n" "stp q10,q11,[sp,#480]\n" \
    "stp q12,q13,[sp,#512]\n" "stp q14,q15,[sp,#544]\n"
#define SVM_HELPER_RESTORE_BANK \
    "ldp q2,q3,[sp,#128]\n" "ldp q4,q5,[sp,#160]\n" \
    "ldp q6,q7,[sp,#192]\n" "ldp q16,q17,[sp,#224]\n" \
    "ldp q18,q19,[sp,#256]\n" "ldp q20,q21,[sp,#288]\n" \
    "ldp q22,q23,[sp,#320]\n" "ldp q24,q25,[sp,#352]\n" \
    "ldp q26,q27,[sp,#384]\n" "ldp q28,q29,[sp,#416]\n" \
    "ldp q30,q31,[sp,#448]\n" "ldp q10,q11,[sp,#480]\n" \
    "ldp q12,q13,[sp,#512]\n" "ldp q14,q15,[sp,#544]\n" \
    "ldp x1,x2,[sp,#0]\n" "ldp x3,x4,[sp,#16]\n" \
    "ldp x5,x6,[sp,#32]\n" "ldp x7,x8,[sp,#48]\n" \
    "ldp x9,x10,[sp,#64]\n" "ldp x11,x12,[sp,#80]\n" \
    "ldp x13,x14,[sp,#96]\n" "ldp x15,x30,[sp,#112]\n" \
    "add sp,sp,#576\n" "ret\n"

// x0=high, x16=low, x17=divisor -> x0=quotient, x16=remainder.
#define SVM_DIV_STUB(name, config) \
__attribute__((naked)) static void name() { \
    __asm__ volatile( \
        SVM_HELPER_SAVE_BANK \
        "mov x1,x16\n" "mov x2,x17\n" "mov x3,#" #config "\n" \
        "bl " SVM_DIV_SYMBOL "\n" "mov x16,x1\n" \
        SVM_HELPER_RESTORE_BANK); \
}
SVM_DIV_STUB(Div8U, 8)
SVM_DIV_STUB(Div16U, 16)
SVM_DIV_STUB(Div32U, 32)
SVM_DIV_STUB(Div64U, 64)
SVM_DIV_STUB(Div8S, 264)
SVM_DIV_STUB(Div16S, 272)
SVM_DIV_STUB(Div32S, 288)
SVM_DIV_STUB(Div64S, 320)
#undef SVM_DIV_STUB
#undef SVM_DIV_SYMBOL

// x0=leaf, x16=subleaf, x17=immutable callback. Its ordinary C ABI is
// {u64 lower,u64 upper} callback(u64 leaf,u64 subleaf) noexcept, returning
// x0/x1. Move the callback before BLR replaces x30 with its return address.
__attribute__((naked)) static void QueryCpuid() {
    __asm__ volatile(
        SVM_HELPER_SAVE_BANK
        "mov x1,x16\n" "mov x16,x17\n" "blr x16\n" "mov x16,x1\n"
        SVM_HELPER_RESTORE_BANK);
}

// x0=state, x16=command, x17=raw64. The semantic helper accesses only
// its x87 subobject. The integer-only closure retains all vector homes and
// the FP environment; this bridge owns GPRs and NZCV.
__attribute__((naked)) static void ExecuteX87() {
    __asm__ volatile(
#if SVM_X87_GPR_ONLY
        "sub sp,sp,#144\n"
        "stp x1,x2,[sp,#0]\n" "stp x3,x4,[sp,#16]\n"
        "stp x5,x6,[sp,#32]\n" "stp x7,x8,[sp,#48]\n"
        "stp x9,x10,[sp,#64]\n" "stp x11,x12,[sp,#80]\n"
        "stp x13,x14,[sp,#96]\n" "stp x15,x30,[sp,#112]\n"
        "mrs x3,nzcv\n" "str x3,[sp,#128]\n"
#else
        SVM_HELPER_SAVE_BANK
#endif
        "mov x1,x16\n" "mov x2,x17\n"
#if defined(__APPLE__)
        "bl _SvmX87Execute\n"
#else
        "bl SvmX87Execute\n"
#endif
#if SVM_X87_GPR_ONLY
        "ldr x3,[sp,#128]\n" "msr nzcv,x3\n"
        "ldp x1,x2,[sp,#0]\n" "ldp x3,x4,[sp,#16]\n"
        "ldp x5,x6,[sp,#32]\n" "ldp x7,x8,[sp,#48]\n"
        "ldp x9,x10,[sp,#64]\n" "ldp x11,x12,[sp,#80]\n"
        "ldp x13,x14,[sp,#96]\n" "ldp x15,x30,[sp,#112]\n"
        "add sp,sp,#144\n" "ret\n"
#else
        SVM_HELPER_RESTORE_BANK
#endif
        );
}
#undef SVM_HELPER_SAVE_BANK
#undef SVM_HELPER_RESTORE_BANK
#else
// Cross-host word-generation targets cannot execute this private ARM ABI.
static void QueryCpuid() { std::abort(); }
static void ExecuteX87() { std::abort(); }
#endif

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned bits, bool sign) noexcept {
#if defined(__aarch64__)
    void(*entry)() = bits == 8 ? (sign ? Div8S : Div8U) : bits == 16 ? (sign ? Div16S : Div16U)
            : bits == 32 ? (sign ? Div32S : Div32U) : (sign ? Div64S : Div64U);
    return reinterpret_cast<std::uintptr_t>(entry);
#else
    // Cross-host word-generation tests never execute ARM64 words.
    (void)bits; (void)sign;
    return reinterpret_cast<std::uintptr_t>(&SvmArm64WideDivide);
#endif
}
std::uintptr_t CpuidHelper() noexcept {
    return reinterpret_cast<std::uintptr_t>(&QueryCpuid);
}
std::uintptr_t X87Helper() noexcept {
    return reinterpret_cast<std::uintptr_t>(&ExecuteX87);
}
}
