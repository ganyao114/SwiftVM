#pragma once

// Clang's AArch64 preserve_all convention keeps x9-x15, x19-x29 and the low
// 128 bits of v8-v31 in the callee. GCC 13 accepts the spelling but ignores
// it, so never advertise the contract there: enabling the matching JIT path
// without a real compiler ABI would silently corrupt live guest state.
#if defined(__aarch64__) && defined(__clang__) && __has_attribute(preserve_all)
#define SVM_HELPER_PRESERVE_ALL __attribute__((preserve_all))
#define SVM_HAS_HELPER_PRESERVE_ALL 1
#else
#define SVM_HELPER_PRESERVE_ALL
#define SVM_HAS_HELPER_PRESERVE_ALL 0
#endif

// ELF local-exec TLS lowers to TPIDR_EL0 plus a fixed offset and is a true
// leaf. Mach-O TLV necessarily calls __tlv_get_addr, so a helper that touches
// thread_local storage must retain the normal caller snapshot on macOS even
// though the compiler supports preserve_all for ordinary leaf functions.
#if SVM_HAS_HELPER_PRESERVE_ALL && defined(__linux__)
#define SVM_HELPER_TLS_IS_LEAF 1
#else
#define SVM_HELPER_TLS_IS_LEAF 0
#endif
