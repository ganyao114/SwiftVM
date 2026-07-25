# Guest threading and TSO verification report

Date: 2026-07-25

## Scope and current status

The x86_64 launcher now has a minimal Linux thread-group model and runs a real
concurrent guest built without libc. Four guest threads execute on four host
`std::thread`s, share one guest address space, synchronize through futexes, and
join through `CLONE_CHILD_CLEARTID`. The smoke test performs 80,000 protected
increments while alternating memory `xchg` and `lock cmpxchg` for the lock and
also performs 80,000 concurrent `lock xadd` operations.

The requested glibc/musl pthread and native x64 qualification is **not complete**.
The sources and build/recording workflow are present, but OrbStack cannot start
the `ubuntu-x64` command because its VM reports:

```text
Unknown Rosetta version: Rosetta-367.3
start VM: VM exited unexpectedly
```

No pthread or native result below is inferred from the raw-clone fallback.

## Phase 1 design

### Runtime ownership model

Before this change, `RunX86Guest` owned one `X86Instance`, one `X86Core`, one
`ThreadContext64`, and one `SyscallHandler`. `X86Instance` owns the
`AddressSpace`; `X86Core` owns a `Runtime`, whose uniform buffer contains the
thread context.

The implemented N-thread shape is:

- Process-wide: one `X86Instance`/`AddressSpace`, one `GuestMemory`, one
  `SyscallProcessState`, one guest page map, one L2 translation table, and one
  set of modules/JIT code caches.
- Per guest thread: one host `std::thread`, one `X86Core`, one `Runtime`, one
  `ThreadContext64`, one syscall handler, TLS bases, TID, robust-list metadata,
  and clear-child-TID address.
- Lifecycle: the parent context is copied at `clone`; the child receives
  `rax=0`, the requested stack, and `fs_base=tls` for `CLONE_SETTLS`. The
  parent receives the allocated guest TID. See
  `source/translator/linux/main.cpp:99-243`.

This follows the useful part of FEX's model—process-wide thread tracking plus a
per-thread CPU state—but not its full signal/scheduler machinery
(`../SwiftVM/.cache/FEX/Source/Tools/LinuxEmulation/LinuxSyscalls/ThreadManager.h:134-203`).
cross86 also creates a per-thread runner and host thread, but additionally
supports Linux host `clone`, process clones, ptrace, and allocator/fault
reinitialization (`../SwiftVM/.cache/cross86/frontends/frontend-linux/src/thread_executor.rs:805-855`,
`:1015-1082`). Those Linux-host process features do not fit the macOS minimal
thread-only scope, so the local `std::thread` model was selected.

### Syscall surface

| Surface | Coverage |
| --- | --- |
| `clone` (x86_64 56) | Thread clones only. Requires `CLONE_VM`, `CLONE_FILES`, `CLONE_SIGHAND`, and `CLONE_THREAD`; accepts `CLONE_FS`, `CLONE_SYSVSEM`, `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`, and obsolete musl `CLONE_DETACHED`. Process clones and unsupported flags return `EINVAL`. |
| `clone3` | Not implemented; returns `ENOSYS` through the generic path, allowing a libc fallback to `clone`. |
| `futex` | `WAIT`, `WAKE`, `WAIT_BITSET`, and `WAKE_BITSET`; `PRIVATE_FLAG` is accepted, val32 is checked atomically before queueing under the futex-table lock, relative/absolute timeouts are supported, and timeout returns `ETIMEDOUT`. No guest signal delivery, therefore no generated `EINTR`. |
| `set_tid_address` | Saves the calling thread's clear-child-TID pointer and returns its guest TID. |
| `set_robust_list` | Records the head/length; robust-list recovery on abnormal thread death is not implemented. |
| `gettid` | Returns the per-thread allocated guest TID. |
| `arch_prctl(ARCH_SET_FS)` | Updates the calling thread's `ThreadContext64::fs_base`; clone TLS is installed before first child execution. |
| `exit` | Terminates only the calling guest thread. |
| `exit_group` | Sets process-wide exit state, wakes all futex waiters, and requests an interrupt from all registered cores. A whole-function JIT loop that never returns to the dispatcher remains a known interruption-latency limitation. |

The syscall dispatch points are in
`source/translator/linux/syscalls.cpp:476-488`, `:591-604`, and `:651`; the
futex implementation is at `:300-444`, and clone validation is at
`:1136-1158`. Child clear-and-wake is at
`source/translator/linux/main.cpp:231-242`.

musl's pthread path needs the clone TLS/TID flags above and waits through
private futex operations. Its timed-wait helper converts absolute API
deadlines to a relative `FUTEX_WAIT|PRIVATE` operation; its join implementation
waits on thread detach state. The local Orb failure prevented confirming the
exact checked-in test binaries with `strace -f`.

### Shared-state synchronization audit

| Shared structure | State before/after this work |
| --- | --- |
| `AddressSpace::modules` | Already protected by its shared mutex (`source/runtime/backend/address_space.cpp:59-77`). |
| `Module::address_node_map` | Already protected by `inner_lock` (`source/runtime/backend/module.cpp:106-175`). |
| Module code allocators and fault table | Already protected by `cache_lock` (`source/runtime/backend/module.cpp:184-279`). |
| L2 `TranslateTable` | Host lookup/publication APIs use a shared mutex (`source/runtime/backend/translate_table.h:38-165`). Publication now uses a release fence before exposing a new key. Generated dispatch remains a deliberately lock-free aligned read; a racing publication may produce a harmless cache miss. |
| Frontend translation and fallback sets | Previously unsynchronized. One coarse `translate_mutex` now serializes decode, IR mutation, JIT publication, function fallback sets, and statistics. It rechecks the cache after taking the lock to prevent duplicate compilation (`source/translator/x86/translator.cpp:367-376`, `:605`). |
| Per-runtime dispatcher | Each guest thread has its own `Runtime` and L1 table. The L2 table and immutable emitted code are shared. |
| Guest mapping intervals | Previously unsynchronized. `GuestMemory` now uses a shared mutex around map tracking and validated reads/writes (`source/translator/linux/guest_memory.cpp:139-218`). Process-wide `brk` and mapping syscalls also use one coarse memory mutex. |
| Futex table/process exit/TIDs | New process-wide state: futex queues are mutex-protected; exit and TID allocation are atomic (`source/translator/linux/syscalls.h:206-253`). |
| SMC tracker | MT SMC now clears the shared L2 and every registered Runtime L1, detaches stale nodes from dispatch visibility, and defers executable/fault-table reclamation with per-Runtime QSBR epochs (`source/runtime/backend/smc_tracker.cpp`). The runtime publishes before cache lookup and becomes quiescent immediately after trampoline return (`source/runtime/backend/runtime.cpp`). `SVM_SMC_MT=0` retains the old diagnostic fallback that unprotects pages and disables tracking. |
| Host fault handlers | Process-global callbacks are registered once; the active runtime is selected from thread-local storage (`source/runtime/backend/runtime.cpp:33`, `:88-106`, `:292-302`). Each running host thread installs its alternate stack. |
| Atomic guest RMW | Memory `xchg`, `lock cmpxchg`, and locked add/sub/xadd were previously ordered load/store sequences rather than atomic operations. They now use exclusive-loop IR in both JIT and interpreter (`source/runtime/frontend/x86/decoder_alu.cc:303-328`, `:392-409`, `:872-922`; `source/runtime/backend/arm64/jit/translator_mem.cpp:488-651`). Other locked RMW families such as `bts/btr/btc`, `inc/dec`, and logical ops still need true atomic lowering before they can be advertised for arbitrary threaded guests. |

### TSO mode plumbing

The existing `Config::tso_mode` was not being installed into the x86
frontend's process-wide mode hook. `X86Instance` now reads
`SVM_TSO_MODE=relaxed|acqrel|hardware`, defaults to Relaxed, stores it in
`Config`, and calls `x86::SetTsoMode` before any decode
(`source/translator/x86/translator.cpp:57-70`, `:346-355`).

## Tests added

- `pthread_mutex_counter.c`: four pthreads, mutex-protected exact counter.
- `tso_spinlock_counter.c`: memory `xchg` spinlock plus plain release store.
- `tso_litmus.c`: persistent-thread SB (1,000,000 iterations by default) and
  MP; `mp_violations != 0` fails.
- `tso_peterson.c`: Peterson mutual exclusion with locked exchange fence.
- `clone_futex_smoke_x86_64.S`: locally buildable clone/futex/join and atomic
  RMW fallback.
- `clone_smc_mt_x86_64.S`: one thread repeatedly patches a page-distinct
  worker while another is executing the retired translation; exact alternating
  values and checksum cover cross-thread invalidation plus deferred reclaim.
- `clone_tso_litmus_x86_64.S`: locally buildable persistent-thread SB+MP
  fallback.

`build_real_tests.sh:35-53` builds raw, glibc, and musl variants and `:61-97` records
native output plus exact exit status.

## Verification results

### Raw-clone mutex/atomic smoke

Each run creates four guest/host threads and checks both counters equal 80,000.

| Mode | Result |
| --- | --- |
| Relaxed | 10/10 passed |
| AcqRel | 10/10 passed |

### Raw-clone SB + MP, 1,000,000 iterations per run

This is a real concurrent SwiftVM result, but not the requested libc/native
qualification result.

| Execution | Run 1 | Run 2 | Run 3 |
| --- | ---: | ---: | ---: |
| SwiftVM Relaxed SB `00` | 972,304 | 976,167 | 971,733 |
| SwiftVM Relaxed MP violations | 29,452 | 25,718 | 33,170 |
| SwiftVM AcqRel SB `00` | 998,861 | 998,478 | 998,903 |
| SwiftVM AcqRel MP violations | 0 | 0 | 0 |
| Native x64 | **Not run — Orb/Rosetta startup blocker** | — | — |

SB `00` is present in AcqRel, so the implementation is not accidentally SC.
AcqRel forbids the tested MP reordering in all three million observations.
Relaxed remains the historical unbarriered mode and is intentionally not
claimed as x86 TSO; its MP failures demonstrate that the litmus detects the
missing ordering.

### Single-thread regression matrix

Both Relaxed and AcqRel produced identical functional output and expected exit
status:

| Guest | Expected/result |
| --- | ---: |
| `hello_x86_64` | 42 |
| `loop_x86_64` | 186 |
| `real_hello_musl_x86_64` | 42 |
| `real_busy_musl_x86_64` | 0 |
| `smc_x86_64` | 99 |
| `bad_pointer_x86_64` | launcher reports guest fault, 1 |
| `func_tests_musl_x86_64` | checksum `9f52b7d59285dbe5`, 101, matches checked-in native result |

The full CMake build succeeds. Nineteen selected non-fuzz directed host tests
pass. Two existing harness/baseline failures remain and do not touch this
work's code paths:

- `Test runtime` aborts at `source/tests/main_case.cpp:184` with
  `std::logic_error("base")`.
- `Flag elimination keeps live pseudo masks and unlinks dead pseudos` fails
  its pre-existing assertion at `source/tests/main_case.cpp:476`.
- x86 fuzz tests cannot initialize Unicorn on this host: SIGILL occurs inside
  `uc->MapMemory`, before `X86Instance::Make`.

## Remaining work

1. Repair/start OrbStack x64, run `build_real_tests.sh x86_64`, preserve
   `strace -f`, and run every glibc/musl pthread binary natively.
2. Run all four pthread tests under SwiftVM Relaxed and AcqRel and add their
   output distributions to the table above.
3. Add atomic lowering for the remaining LOCK-capable RMW instruction
   families and an unaligned atomic slow path (AArch64 exclusives require
   natural alignment whereas x86 locked operations permit unaligned memory).
4. Replace the coarse translation lock with per-location compile ownership
   only after profiling; correctness currently takes priority.
5. Extend SMC handling to the interpreter if decoded IR is ever expected to
   observe guest code mutations while `SVM_ENABLE_JIT=0`; JIT MT SMC is now
   covered by QSBR reclamation and `clone_smc_mt_x86_64`.
