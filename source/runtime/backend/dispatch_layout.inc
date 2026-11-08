#pragma once

// The directory layout is shared by cold C++ lookup and native backends.
#define SVM_DISPATCH_HASH_SHIFT 13
#define SVM_DISPATCH_SLOT_SHIFT 5
#define SVM_DISPATCH_OCCUPIED_OFFSET 24
#define SVM_DISPATCH_CODE_ID_OFFSET 16

// Thread-private ready-entry cache, shared by the native backend ABIs.
#define SVM_DISPATCH_L1_BITS 20
#define SVM_DISPATCH_L1_ENTRY_SHIFT 4
#define SVM_DISPATCH_CTX_L1 1032
