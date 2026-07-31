# W77: Linux function-fingerprint golden drift audit

Date: 2026-08-01  
Checkout: `w77-fp-drift` at `afbf1ec`  
Scope: read-only investigation; no source or golden changes

## Conclusion

The current Linux-vs-golden failure is **not a host-address/ASLR byte drift and is not one root cause**. It is the deterministic sum of two platform inputs:

1. The function-fingerprint guests inherit the host `fstat(stdout)` result. Darwin `/dev/null` reports `st_blksize=65536`, while Orb Linux reports `4096`. Static glibc's `_IO_file_doallocate` consequently compiles a different pair of guest basic blocks in `real_hello`, `real_busy`, and `func_tests`.
2. `FlagsCfinvEnabled()` detects FEAT_FlagM only on Apple. Orb advertises `flagm flagm2`, but the Linux build unconditionally returns false. The same `cmp; adc` block therefore uses the carry-normalization fallback and contains three more optimized IR instructions in `real_busy` and `func_tests`.

Both paths preserve guest semantics. The first is legitimate host-dependent syscall data leaking into an execution-driven coverage fingerprint. The second is a Linux feature-detection omission which costs code quality but is not a guest-correctness failure.

There is also a counting correction. On the requested current binary/base I cannot reproduce “real_busy 14 lines”. The fresh full diff has **18 changed content lines** (`9` removals plus `9` additions), covering `real_hello`, `real_busy`, and `func_tests`. Ubuntu and wine-ci reproduce the same diff, and the current golden is byte-identical to the file at `c905307`. The historical “14 lines” was therefore a stale or differently-counted observation; the current evidence below is used without trimming it to fit that number.

## Reproduction

The requested existing master binary was used; no golden was generated:

```sh
orb -m ubuntu bash -lc '
  set -o pipefail
  /private/tmp/w64/source/translator/linux/tests/run_func_fingerprint_tests.sh \
    /tmp/svm-build/source/translator/linux/svm_translator_linux \
    2>&1 | tee /tmp/w77-golden-check.log
  rc=${PIPESTATUS[0]}
  echo W77_RC=$rc
'
```

Result:

```text
guest staging: /tmp/svm_fp_guests (fixed argv[0]/AT_EXECFN, copies not symlinks)
self-consistency: OK (4400 function units over 11 guests, host_bytes included)
FAIL: function-mode emission fingerprint differs from .../func_fingerprint_golden.txt
W77_RC=1
```

The harness prints only its first 40 diff lines. I emitted the same normalized fingerprint without `--update`, saved it as `/tmp/w77-linux-fingerprint.txt`, and ran:

```sh
orb -m ubuntu bash -lc '
  svm=/tmp/svm-build/source/translator/linux/svm_translator_linux
  stage=/tmp/svm_fp_guests
  tests=/private/tmp/w64/source/translator/linux/tests
  guests="hello loop basic_coverage_smoke random_smoke vec_float_nan_pressure \
          real_hello real_hello_musl real_busy real_busy_musl \
          func_tests func_tests_musl"
  mkdir -p "$stage"
  for g in $guests; do
    rm -f "$stage/${g}_x86_64"
    cp -f "$tests/${g}_x86_64" "$stage/${g}_x86_64"
    raw=$(SVM_PROF=2 SVM_FUNC_BASE=1 SVM_JIT_CACHE= \
          "$svm" "$stage/${g}_x86_64" 2>&1 >/dev/null)
    printf "%s\n" "$raw" \
      | sed -n "s/^\[svm-unit\] /$g /p" \
      | sed "s/ host=[0-9]*$//" | sort
    printf "%s\n" "$raw" \
      | sed -n "s/^\[svm-prof\] \(func_units=.*\)$/$g TOTALS \1/p" \
      | sed "s/ host_bytes=[0-9]*//; s/ pool_bytes=[0-9]*//"
  done > /tmp/w77-linux-fingerprint.txt

  diff -u \
    /private/tmp/w64/source/translator/linux/tests/func_fingerprint_golden.txt \
    /tmp/w77-linux-fingerprint.txt | tee /tmp/w77-full.diff
'
```

The complete diff is:

```diff
--- /private/tmp/w64/source/translator/linux/tests/func_fingerprint_golden.txt
+++ /tmp/w77-linux-fingerprint.txt
@@ -782,11 +782,11 @@
 real_hello pc=0x438c8c ir=9
 real_hello pc=0x438c90 ir=18
 real_hello pc=0x438ca4 ir=16
+real_hello pc=0x438cb5 ir=10
+real_hello pc=0x438cbd ir=12
 real_hello pc=0x438cc5 ir=17
 real_hello pc=0x438cd6 ir=20
 real_hello pc=0x438cf2 ir=29
-real_hello pc=0x438d00 ir=13
-real_hello pc=0x438d0d ir=12
 real_hello pc=0x438d20 ir=47
 real_hello pc=0x438d58 ir=22
 real_hello pc=0x438d6c ir=16
@@ -1027,7 +1027,7 @@
 real_hello pc=0x47e059 ir=15
 real_hello pc=0x47e05f ir=1
 real_hello pc=0x47f074 ir=20
-real_hello TOTALS func_units=927 block_units=0 decoded_blocks=927 ir_insts=21681
+real_hello TOTALS func_units=927 block_units=0 decoded_blocks=927 ir_insts=21678
 real_hello_musl pc=0x401000 ir=17
 real_hello_musl pc=0x401010 ir=17
 real_hello_musl pc=0x40101c ir=7
@@ -2067,7 +2067,7 @@
 real_busy pc=0x439941 ir=10
 real_busy pc=0x43994c ir=22
 real_busy pc=0x43995d ir=24
-real_busy pc=0x439970 ir=45
+real_busy pc=0x439970 ir=48
 real_busy pc=0x43998b ir=10
 real_busy pc=0x439996 ir=16
 real_busy pc=0x439998 ir=10
@@ -2124,11 +2124,11 @@
 real_busy pc=0x44158c ir=9
 real_busy pc=0x441590 ir=18
 real_busy pc=0x4415a4 ir=16
+real_busy pc=0x4415b5 ir=10
+real_busy pc=0x4415bd ir=12
 real_busy pc=0x4415c5 ir=17
 real_busy pc=0x4415d6 ir=20
 real_busy pc=0x4415f2 ir=29
-real_busy pc=0x441600 ir=13
-real_busy pc=0x44160d ir=12
 real_busy pc=0x441620 ir=47
 real_busy pc=0x441658 ir=22
 real_busy pc=0x44166c ir=16
@@ -3602,7 +3602,7 @@
 func_tests pc=0x419be1 ir=10
 func_tests pc=0x419bec ir=22
 func_tests pc=0x419bfd ir=24
-func_tests pc=0x419c10 ir=45
+func_tests pc=0x419c10 ir=48
 func_tests pc=0x419c2b ir=10
 func_tests pc=0x419c36 ir=16
 func_tests pc=0x419c41 ir=9
@@ -3881,11 +3881,11 @@
 func_tests pc=0x4596dc ir=9
 func_tests pc=0x4596e0 ir=18
 func_tests pc=0x4596f4 ir=16
+func_tests pc=0x459705 ir=10
+func_tests pc=0x45970d ir=12
 func_tests pc=0x459715 ir=17
 func_tests pc=0x459726 ir=20
 func_tests pc=0x459742 ir=29
-func_tests pc=0x459750 ir=13
-func_tests pc=0x45975d ir=12
 func_tests pc=0x459770 ir=47
 func_tests pc=0x4597a8 ir=22
 func_tests pc=0x4597bc ir=16
```

Reproducibility checks:

- wine-ci produced the same normalized diff.
- `git diff --no-index <(git show c905307:source/translator/linux/tests/func_fingerprint_golden.txt) source/translator/linux/tests/func_fingerprint_golden.txt` produced no output.
- Ten additional Ubuntu harness invocations all reported `self-consistency: OK`; this is 20 same-binary emissions with `host_bytes` retained.

## Line-by-line classification

The golden comparison cannot contain a `host_bytes` difference. In `emit_fingerprint()`, the check path removes ` host=[0-9]*` from every unit and removes `host_bytes`/`pool_bytes` from totals before diffing (`run_func_fingerprint_tests.sh:97-115`). Host byte counts are retained only for the preceding same-binary self-consistency check.

| Changed records | Class | Exact meaning | Root cause |
|---|---|---|---|
| `real_hello 0x438cb5/0x438cbd` replaces `0x438d00/0x438d0d` | Guest unit-set / IR coverage | Linux compiles the `malloc(st_blksize)` path; Darwin compiled the fixed `malloc(8192)` path | `/dev/null` `st_blksize` |
| `real_hello TOTALS 21681 -> 21678` | Guest IR total | `(10+12) - (13+12) = -3` | Same as above |
| `real_busy 0x439970 45 -> 48` | Same guest PC, IR shape | Carry fallback has three more optimized IR instructions | Linux FlagM detection missing |
| `real_busy 0x4415b5/0x4415bd` replaces `0x441600/0x44160d` | Guest unit-set / IR coverage | Same `_IO_file_doallocate` path switch | `/dev/null` `st_blksize` |
| `func_tests 0x419c10 45 -> 48` | Same guest PC, IR shape | Same `cmp; adc` lowering switch | Linux FlagM detection missing |
| `func_tests 0x459705/0x45970d` replaces `0x459750/0x45975d` | Guest unit-set / IR coverage | Same `_IO_file_doallocate` path switch | `/dev/null` `st_blksize` |

`real_busy` and `func_tests` totals do not appear in the full diff because each has `-3` IR from the glibc path switch and `+3` IR from the FlagM fallback; the independent changes cancel exactly. This cancellation is why the totals alone are insufficient for diagnosis.

For diagnostic purposes only, raw unit traces showed:

| Unit family | Darwin golden-side raw host bytes | Linux raw host bytes | Interpretation |
|---|---:|---:|---|
| `_IO_file_doallocate` selected first unit | `104` at the Darwin PC | `92` at the Linux PC | Different guest block, not a same-unit address immediate |
| `_IO_file_doallocate` selected second unit | `120` | `120` | Different guest block |
| `__printf_buffer` `cmp; adc` unit | `344` | `348` | Instruction/lowering selection, not an address |

These numbers are not part of the golden diff.

## Root A: host `st_blksize` changes guest coverage

The host facts are deterministic:

```sh
$ stat -f 'mac /dev/null blksize=%k mode=%p' /dev/null
mac /dev/null blksize=65536 mode=20666

$ orb -m ubuntu stat -c 'linux /dev/null blksize=%o mode=%f' /dev/null
linux /dev/null blksize=4096 mode=21b6
```

`SyscallHandler::WriteGuestStat()` copies `h.st_blksize` directly into the x86-64 guest structure (`source/translator/linux/syscalls.cpp:2150-2168`; the Arm64 structure does the same at 2170-2187). The fingerprint harness redirects guest stdout to `/dev/null`, so static glibc observes the host value.

The three affected binaries contain the same `_IO_file_doallocate` logic at relocated addresses. For `real_hello`:

```asm
438ca4: mov 0x38(%rsp),%rbp       # stat.st_blksize
438ca9: lea -0x1(%rbp),%rax
438cad: cmp $0x1ffe,%rax
438cb3: ja  438d00
438cb5: mov %rbp,%rdi             # Linux: malloc(4096)
438cb8: call __libc_malloc
438cbd: mov %rax,%rsi
...
438d00: mov $0x2000,%ebp          # Darwin: clamp/fallback to 8192
438d05: mov %rbp,%rdi
438d08: call __libc_malloc
438d0d: mov %rax,%rsi
```

`addr2line -f` identifies all six path-specific pairs as `_IO_file_doallocate`:

```text
real_hello  0x438cb5 / 0x438d00
real_busy   0x4415b5 / 0x441600
func_tests  0x459705 / 0x459750
```

### Minimal causal experiment

I built a temporary Orb-only `LD_PRELOAD` probe outside the checkout. It delegates `fstat`, then changes only `stdout`'s returned block size to Darwin's value:

```c
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int (*fstat_fn)(int, struct stat *);
int fstat(int fd, struct stat *st) {
    static fstat_fn next;
    if (!next) next = (fstat_fn)dlsym(RTLD_NEXT, "fstat");
    int rc = next(fd, st);
    if (rc == 0 && fd == STDOUT_FILENO) st->st_blksize = 65536;
    return rc;
}
```

```sh
orb -m ubuntu bash -lc '
  cc -shared -fPIC -O2 /private/tmp/w77_fstat_preload.c -ldl \
    -o /tmp/w77-fstat-preload.so
  LD_PRELOAD=/tmp/w77-fstat-preload.so \
    /private/tmp/w64/source/translator/linux/tests/run_func_fingerprint_tests.sh \
    /tmp/svm-build/source/translator/linux/svm_translator_linux
'
```

Result: all twelve changed `_IO_file_doallocate` unit records disappeared, and the `real_hello` total matched. The only remaining differences were the two FlagM units and their now-unmasked `+3` totals. This is an exact intervention on the proposed cause.

## Root B: Linux does not enable the FlagM lowering

Both same-PC units are in static glibc's `__printf_buffer` and contain the same sequence:

```asm
cmp $0x1,%r12d
adc $0xffffffff,%eax
```

The PCs are `real_busy:0x439970` and `func_tests:0x419c10`. In `ArithWithFlags()`, a known inverted carry polarity uses one `InvertCarry` IR when `FlagsCfinvEnabled()` is true; otherwise it materializes carry with `TestFlags`, `Xor`, `LoadImm`, `Add`, and `SaveFlags` before `Adc` (`decoder_alu.cc:175-215`). After optimization this is the observed `45 -> 48` IR delta.

The platform probe is asymmetric (`decoder.cc:1149-1164`): Apple AArch64 queries `hw.optional.arm.FEAT_FlagM`; every other build returns false. That does not reflect the Orb CPU:

```text
Darwin: hw.optional.arm.FEAT_FlagM: 1
Orb:    /proc/cpuinfo Features contains flagm and flagm2
```

### Minimal causal experiment

A clean Mac build from `afbf1ec` in `/tmp/w77-build` matched the golden exactly: 4400 units, rc 0:

```sh
cmake -S /private/tmp/w64 -B /tmp/w77-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/w77-build --target svm_translator_linux -j8
source/translator/linux/tests/run_func_fingerprint_tests.sh \
  /tmp/w77-build/source/translator/linux/svm_translator_linux
```

Forcing only the existing feature switch off reproduced the Linux IR-shape component exactly:

```sh
SVM_FLAGS_CFINV=0 \
  source/translator/linux/tests/run_func_fingerprint_tests.sh \
  /tmp/w77-build/source/translator/linux/svm_translator_linux
```

Result:

```diff
-real_busy pc=0x439970 ir=45
+real_busy pc=0x439970 ir=48
-real_busy TOTALS ... ir_insts=27602
+real_busy TOTALS ... ir_insts=27605
-func_tests pc=0x419c10 ir=45
+func_tests pc=0x419c10 ir=48
-func_tests TOTALS ... ir_insts=26987
+func_tests TOTALS ... ir_insts=26990
```

`SVM_DUMP_IR=1` at `0x439970` additionally showed the expected direct substitution:

```text
FlagM on:   InvertCarry; Adc; SaveFlags
FlagM off:  TestFlags CF; Xor; LoadImm(~0); Add; SaveFlags CF; Adc; SaveFlags
```

## Relation to `c316570` and pointer audit

This drift is not in the `c316570` family:

- `c316570` fixed a same-binary, cross-process **host byte count** nondeterminism. VIXL `Mov(ip, helper_address)` could omit zero 16-bit chunks depending on ASLR, producing 2/3-instruction materializations. The fix uses fixed `movz + 3*movk` (`translator_control.cpp:341-353`).
- This golden comparison removes host byte counts before comparison. The changed records are guest PCs and IR counts, and both causes are deterministic platform inputs.
- Ten fresh Linux checks (20 emissions) retained host bytes in the self-consistency phase and all passed.

A narrow source audit did find other variable-width host-pointer materializations, notably `&unaligned_atomic_lock` in `translator_mem.cpp:63-77`, `&HostMemMove` at `translator_mem.cpp:1050`, and trampoline-local pointers in `trampolines.cpp:377-379`. They are **not present in or causal for these changed golden records**, and no drift was observed in this corpus. They remain a separate latent self-consistency-hardening topic: if covered by a future corpus, direct JIT-embedded host addresses should follow the fixed-width/relocation-scannable contract established by `c316570`. Guest PCs such as `EmitSetLocation()` immediates are not host pointers and should not be included in that cleanup.

Therefore the answer to “is there another address/pointer missed in these 14 lines?” is **no**. The broader source answer is “there are other candidate sites, but this diff provides no evidence that any of them is currently drifting.”

## Answers and recommended repair

### 1. Are all changed lines one root cause?

No. They are two independent causes repeated in duplicated static-glibc code:

- host-derived `st_blksize` changes executed guest blocks in three guests;
- missing Linux FlagM detection changes IR instruction selection at two same-PC units.

The independent probes each removed/reproduced exactly their assigned subset.

### 2. Can one Mac-generated golden be portable to Linux?

Not under the current execution-driven contract. Even after Linux FlagM detection is corrected, the `_IO_file_doallocate` path difference remains as long as the sink is `/dev/null` and Darwin/Linux expose different `st_blksize`. Conversely, CPU feature differences can legitimately alter IR/lowering shape across Linux hosts too. Thus the present Mac golden will deterministically fail on the present Linux setup; it is not a universal cross-platform golden.

### 3. Recommended repair

Recommended sequence:

1. **Fix Linux Arm64 feature detection** independently: populate `Arm64Features::FlagM` from `getauxval(AT_HWCAP) & HWCAP_FLAGM` and `Arm64Features::AXFlag` from `AT_HWCAP2/HWCAP2_FLAGM2` in `DetectArm64Features()`. Have `X64Decoder` retain/use that passed feature bitmap for the CFINV decision, with `SVM_FLAGS_CFINV=0` remaining the override, rather than doing a second Apple-only platform probe in `FlagsCfinvEnabled()`. This removes the avoidable Linux-vs-Mac code-quality mismatch on capable hosts. It does not make a universal golden valid on hosts that genuinely lack FlagM.
2. **Shard golden profiles by execution environment**, at minimum Darwin vs Linux, and record the relevant Arm64 feature bitmap/profile in the fixture identity. Keep the current `--against` build-to-build mode as the authoritative A/B gate.
3. Longer term, if a single canonical golden is required, make the fingerprint workload hermetic at the syscall/input boundary (including output-sink stat metadata) and pin a canonical CPU feature profile. That is a test-fixture design change, not a production syscall-semantics change.

Do **not** use a PC ignore list. It would suppress exactly the unit-set and IR-shape regressions this gate is meant to catch, is brittle when the guest corpus moves, and would have hidden the Linux FlagM omission. Do **not** globally fake `st_blksize` in production merely to satisfy the test. Do **not** regenerate the current golden and call it portable; that would only exchange which platform fails.

Between the offered choices, platform/config sharding is the correct gate repair. `c316570`-style fixed materialization remains appropriate only for actual host-address host-byte nondeterminism, which this investigation did not find in the golden drift.

## Artifacts and repository state

Diagnostic artifacts were written only under `/tmp` or `/private/tmp` (`/tmp/w77-*.log`, `/tmp/w77-full.diff`, `/tmp/w77-linux-fingerprint.txt`, and the temporary preload probe). The checkout change for W77 is this report alone. Neither source nor `func_fingerprint_golden.txt` was modified or regenerated.
