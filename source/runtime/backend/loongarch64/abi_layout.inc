#pragma once

// One NativeRunContext owner, addressed through its embedded guest state.
// These offsets are checked against the C++ layout by the platform binding.
#define SVM_LA_STATE_REG 22
#define SVM_LA_FLAGS_REG 31
#define SVM_LA_CTX_OPAQUE 0
#define SVM_LA_CTX_RESOLVE 8
#define SVM_LA_CTX_SLOTS 16
#define SVM_LA_CTX_MASK 24
#define SVM_LA_CTX_ACTIVE 32
#define SVM_LA_CTX_ANCHOR 40
#define SVM_LA_CTX_DISPATCHES 48
#define SVM_LA_CTX_HITS 56
#define SVM_LA_CTX_FLAGS_OFFSET 72
#define SVM_LA_CTX_PC_OFFSET 80
#define SVM_LA_CTX_HOST_FCSR 104
#define SVM_LA_CTX_GUEST_FCSR 120
#define SVM_LA_CTX_FP_ENVIRONMENT 128
#define SVM_LA_CTX_PROBES 136
#define SVM_LA_CTX_PREPARE_ATOMIC 152
#define SVM_LA_CTX_STATE 160
#define SVM_LA_CTX_REL(field) ((field) - SVM_LA_CTX_STATE)
#define SVM_LA_ROOT_FRAME_BYTES 160
