# A2 func_tests fallback audit

One existing diagnostic execution: exit101 and native stdout matched. No timing is a QoR result.

278 attempts = 230 Function + 48 Block retries; 230 units published. Failures are 30 formation + 18 frontend emission; optimizer/backend failures are zero.

| Stage / actual instruction | Count | Decoded / emitted prefix instructions | Born prefix nodes |
|---|---:|---:|---:|
| OverlappingTarget @0x4071c9 (_int_free) | 25 | 19250 | 0 |
| xend @0x45d78a (__lll_unlock_elision) | 12 | 6444 | 18492 |
| OverlappingTarget @0x4082c7 (_int_malloc) | 5 | 645 | 0 |
| rdtsc @0x4230b3 (__pthread_mutex_lock) | 4 | 1084 | 3233 |
| hlt @0x401825 (_start) | 1 | 12 | 39 |
| xgetbv @0x40310b (update_active.constprop.0) | 1 | 75 | 217 |

The 30 formation failures are valid LOCK/non-LOCK shared entries:

```text
4071c6 JE 4071c9; 4071c8 F0 48 0F B1 37  LOCK CMPXCHG [rdi],rsi
                       4071c9 48 0F B1 37     CMPXCHG [rdi],rsi
4082c4 JE 4082c7; 4082c6 F0 4C 0F B1 07  LOCK CMPXCHG [rdi],r8
                       4082c7 4C 0F B1 07     CMPXCHG [rdi],r8
```

DecodeFunction admits the taken target and fallthrough, then the LOCK instruction crosses the next admitted start. Its existing non-overlapping slice contract rejects this at decode.cpp:160 (`OverlappingTarget`); queue() also rejects a later target inside an existing instruction at :88. The two streams differ in atomicity and cannot be merged as one instruction. These are not generic invalid encodings.

XEND is in guarded `__lll_unlock_elision`: TEST/JNE skips XEND when `[rdi]!=0`; RDTSC is in the mutex spin-backoff source block. They are unsupported instructions in formed coverage, not evidence that those instructions executed. HLT is the `_start` noreturn-call continuation. XGETBV is in CPU feature initialization.

Formation failures paid 19,895 decoded instructions / 83,811 bytes / 19,925 byte-decoder calls and built no IR. Emission failures paid 7,615 emitted-prefix instructions / 21,981 born nodes. Runtime's 40,547 accepted-front nodes exclude these 21,981 nodes: actual born total is 62,528. Failed-prefix operand words are unavailable (no extra scan).

This preserves A2 as the rejected speculative-continuation version. It does not claim rollback, overlap-IR support, a new trap frontier, or production changes.
