#pragma once

// One NativeRunContext owner, addressed through its embedded guest state.
// These offsets are checked against the C++ layout by the platform binding.
#define SVM_RV_STATE_REG 9
#define SVM_RV_FLAGS_REG 27
#define SVM_RV_CTX_OPAQUE 0
#define SVM_RV_CTX_RESOLVE 8
#define SVM_RV_CTX_SLOTS 16
#define SVM_RV_CTX_MASK 24
#define SVM_RV_CTX_ACTIVE 32
#define SVM_RV_CTX_ANCHOR 40
#define SVM_RV_CTX_DISPATCHES 48
#define SVM_RV_CTX_HITS 56
#define SVM_RV_CTX_FLAGS_OFFSET 72
#define SVM_RV_CTX_PC_OFFSET 80
#define SVM_RV_CTX_HOST_FCSR 104
#define SVM_RV_CTX_GUEST_FCSR 120
#define SVM_RV_CTX_FP_ENVIRONMENT 128
#define SVM_RV_CTX_PROBES 136
#define SVM_RV_CTX_PREPARE_ATOMIC 152
#define SVM_RV_CTX_STATE 160
#define SVM_RV_CTX_REL(field) ((field) - SVM_RV_CTX_STATE)
#define SVM_RV_ROOT_FRAME_BYTES 208
