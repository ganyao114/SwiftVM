# W85: CoreMark post-pin gap re-attribution

Date: 2026-08-01  
Tree: `w85-coremark-reattr`, `e63b53ccb89534696e7883736ecebbbc663e79a3`  
Measurement host: orb `ubuntu` (AArch64 Linux)

## 1. Decision summary

The W36 attribution is no longer representative of current master.

- In the current 150k run, `SVM_RA_HOT_COALESCE` accounts for
  `348,174,710,727` entry-weighted host instructions. Move/width bridges are
  `35.102447%`; dynamic spill is exactly zero; NaN guards are effectively zero;
  the conservative state-coalescing opportunity is only `0.077356%`.
- The top three guest entries cover `18.104962%` of that mechanical total. After
  removing cold link records/tails, their hot mainlines are 87 SVM instructions
  versus 36 FEX instructions. The 51-instruction gap is 29 move/width (56.86%),
  15 flag normalization/materialization (29.41%), one unpinned GPR-state load
  (1.96%), and six semantic/addressing instructions (11.76%). There is no spill
  or NaN component.
- W81 P1 addresses only part of this shape. At concrete top-1 `0x402680`, its
  audited six-instruction flag sink plus two-instruction safepoint is a net four
  instructions: SVM 19 -> 15, while FEX is 8. It does not address the 28/44
  move-heavy matrix body at `0x402df8`, nor the non-self-edge flag materialization
  at `0x402814`.
- Function-family weighting is almost tied between list (`35.645978%`) and state
  (`35.453073%`); together they are `71.099050%`. Matrix is `19.459715%`, CRC is
  `9.433541%`. Thus there is not one isolated CRC loop to fix: list/state are the
  dominant footprint, while matrix contains the clearest independent width-copy
  project.

Recommendation: finish W81 and remeasure, but open a separate audit-first
integer width/copy canonicalization item centered on `0x402df8`. Do not reopen
GPR caching, spill work, or NaN work for CoreMark. A broader cross-edge flags
representation project is potentially useful but remains a high-risk W30/W67
contract change and should wait for W81's safepoint infrastructure.

## 2. Measurement contract and correctness

### 2.1 Build

```sh
orb -m ubuntu bash -lc '
  cmake -S /private/tmp/w66 -B /tmp/w85-build -DCMAKE_BUILD_TYPE=Release &&
  cmake --build /tmp/w85-build --target svm_translator_linux -j8'
```

The measured defaults are the post-pin defaults in this tree: PIN_EXT level 2
and its scratch-XPOOL bundle are ON by default; PIN_EXT level 3 remains opt-in.
No pin/flags override was supplied to any SVM run.

### 2.2 Accepted CoreMark runs

Every run used for the tables below passed the five required CRCs and emitted
`Correct operation validated`.

These wall times are validation diagnostics, not a new SVM/FEX performance
A/B: the SVM runs contain probes/dumps/gdb and the FEX run is deliberately
CPU-throttled. The performance standing under investigation remains baseline4
SVM/FEX = 0.403; this report re-attributes its current code shape.

| purpose | engine/config | iterations | reported time | result |
|---|---|---:|---:|---|
| W71 aggregate/top-N | SVM + `SVM_RA_HOT_COALESCE` | 150,000 | 20.194 s | validated |
| byte/host dump | SVM + `SVM_VIXL_HOST_DUMP=1` | 150,000 | 12.228 s | validated |
| all-entry family weighting | SVM probe under gdb | 150,000 | 17.711 s | validated |
| same-PC live capture | FEX, multiblock OFF, ABI-local flags ON | 150,000 | 12.844 s | validated |

The FEX capture was pinned to CPU0 alongside one CPU0 load process solely to
cross CoreMark's 10-second validation floor. This changes neither the captured
JIT bytes nor the entry PCs. The accepted raw output is:

```text
Total time (secs): 12.844000
seedcrc          : 0xe9f5
[0]crclist       : 0xe714
[0]crcmatrix     : 0x1fd7
[0]crcstate      : 0x8e3a
[0]crcfinal      : 0x25b5
Correct operation validated. See README.md for run and reporting rules.
```

The three SVM runs have the same five lines and validation result. Evidence:

- `/tmp/w85-coremark-hot/{stdout.txt,hot.txt}`;
- `/tmp/w85-svm-dump/{stdout.txt,stderr.txt,402680.asm,402df8.asm,402814.asm}`;
- `/tmp/w85-all/{gdb.txt,hot.txt}`;
- `/tmp/w85-fex-live-kernels/{stdout.txt,capture.txt,fex-*.bin,fex-*.asm}`.

## 3. Whole-program W71 attribution

Command:

```sh
orb -m ubuntu bash -lc '
  SVM_RA_HOT_COALESCE=/tmp/w85-coremark-hot/hot.txt \
  /tmp/w85-build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64 \
  0x0 0x0 0x66 150000 7 1 2000 \
  >/tmp/w85-coremark-hot/stdout.txt \
  2>/tmp/w85-coremark-hot/stderr.txt'
```

Raw aggregate:

```text
[svm-hot-coalesce] units=1697 versions=1697 executed_units=1697 overflow=0
entries=9774083987 host_dynamic=348174710727
spill_reloads=0 spill_writebacks=0 spill_dynamic=0 spill_pct=0.000000
move_dynamic=122217844285 move_pct=35.102447
nan_guard_dynamic=14 nan_pct=0.000000
state_sequences=54 state_pairs=55 state_same_offset=69
state_saved_dynamic=269334215 state_pct=0.077356
```

The probe's `state_pct` is a conservative **saved-instruction candidate**, not
all state traffic. It can overlap mnemonic classes, so it is reported as a
side-band ceiling and must not be added to the disjoint residual.

| mechanical tag | dynamic count | share of `host_dynamic` | interpretation |
|---|---:|---:|---|
| move/width bridge | 122,217,844,285 | 35.102447% | `mov/fmov/umov/ins`, extensions, `ubfx/bfxil`, and zero shifts |
| spill reload/writeback | 0 | 0% | no CoreMark dynamic spill work |
| NaN guard | 14 | ~0% (`4.0e-9%`) | startup/non-hot noise only |
| other, excluding the three tags above | 225,956,866,428 | 64.897553% | memory, ALU, flags, control, dispatch, etc. |
| state merge candidate, side-band | 269,334,215 | 0.077356% | conservative coalescing upper bound, not an additive class |

This is an instruction-count attribution, not a PMU cycle attribution. Loads,
branches, and dependent ALU chains have different costs.

### 3.1 Top three

| rank | guest PC | guest region | entries | SVM full static | host dynamic | whole-program share | move tag |
|---:|---:|---|---:|---:|---:|---:|---:|
| 1 | `0x402680` | `core_bench_list`, list reversal self-loop | 887,400,000 | 28 | 24,847,200,000 | 7.136417% | 9 / 32.142857% |
| 2 | `0x402df8` | `matrix_mul_matrix_bitextract` inner loop | 388,800,000 | 53 | 20,606,400,000 | 5.918408% | 30 / 56.603774% |
| 3 | `0x402814` | `core_bench_list`, list-search compare | 418,650,000 | 42 | 17,583,300,000 | 5.050137% | 13 / 30.952381% |
| | **sum** | | | | **63,036,900,000** | **18.104962%** | |

`full static` includes cold fallthrough/link tails. The same-PC comparison in
the next section instead counts only the actually traversed hot body through
its terminal branch.

## 4. Same-PC SVM versus FEX code

### 4.1 Capture procedure

SVM bytes came from:

```sh
orb -m ubuntu bash -lc '
  cd /tmp/w85-svm-dump &&
  env -u SVM_JIT_CACHE SVM_VIXL_HOST_DUMP=1 \
  /tmp/w85-build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64 \
  0x0 0x0 0x66 150000 7 1 2000 >stdout.txt 2>stderr.txt'
```

For FEX, `/private/tmp/w85-fex-capture.py` launched the installed
`/usr/bin/FEXInterpreter`, polled `/tmp/perf-PID.map`, stopped its child after
all target offsets appeared, and as the parent used `os.pread` on
`/proc/PID/mem`. It then resumed the process and waited for normal validation.
The aligned single-block configuration and accepted invocation were:

```sh
export FEX_MULTIBLOCK=0 FEX_ABILOCALFLAGS=1 FEX_BLOCKJITNAMING=1
taskset -c 0 yes >/dev/null &
load_pid=$!
trap 'kill "$load_pid" 2>/dev/null || true' EXIT
taskset -c 0 python3 /private/tmp/w85-fex-capture.py
```

Raw perf-map evidence:

```text
0x7fffe4e1e564 6c .../coremark_x64+0x2680 (0x7fffe4e1e564)
0x7fffe4e21440 9c .../coremark_x64+0x2df8 (0x7fffe4e21440)
0x7fffe4e25440 84 .../coremark_x64+0x2814 (0x7fffe4e25440)
```

FEX map sizes include link records and inline metadata; those data words are
not counted as executed instructions. `FEX_MULTIBLOCK=0` is used only to align
guest block boundaries, not to restate the baseline4 wall-time ratio.

### 4.2 Mainline counts and disjoint classification

| PC / engine | hot mainline | GPR state | spill | move/width | NaN | flags normalization/materialization | semantic + guest memory/control |
|---|---:|---:|---:|---:|---:|---:|---:|
| `0x402680` SVM | 19 | 0 | 0 | 5 | 0 | 9 | 5 |
| `0x402680` FEX | 8 | 0 | 0 | 4 | 0 | 0 | 4 |
| `0x402df8` SVM | 44 | 0 | 0 | 28 | 0 | 0 | 16 |
| `0x402df8` FEX | 20 | 0 | 0 | 5 | 0 | 2 | 13 |
| `0x402814` SVM | 24 | 1 | 0 | 5 | 0 | 11 | 7 |
| `0x402814` FEX | 8 | 0 | 0 | 0 | 0 | 3 | 5 |
| **sum SVM** | **87** | **1** | **0** | **38** | **0** | **20** | **28** |
| **sum FEX** | **36** | **0** | **0** | **9** | **0** | **5** | **22** |
| **gap** | **51** | **1 (1.96%)** | **0** | **29 (56.86%)** | **0** | **15 (29.41%)** | **6 (11.76%)** |

This table is semantically disjoint. It therefore differs intentionally from
W71's mnemonic tag: for example a `bfxil x26,...` in the flags pack is placed
under flags here even though W71 also recognizes its mnemonic as a width bridge.

#### `0x402680`: W81-covered self-backedge

Guest body:

```text
402680 mov rdx,rbx
402683 mov (rbx),rdx
402686 mov rcx,(rbx)
402689 mov rbx,rcx
40268c test rdx,rdx
40268f jne 402680
```

SVM's taken path has five register copies, two guest-memory operations, the
`ands`, and nine flag-pack instructions before `b.eq; b back`:

```text
mrs x8,nzcv
and x26,x26,...
and x8,x8,#0xc0000000
orr x26,x26,x8
bfxil x26,x6,#0,#8
mov w6,#0
strb w6,[x28,#1360]
and x26,x26,...
bfc x26,#26,#1
```

FEX keeps the list registers live and uses:

```text
mov x7,x6; ldr x20,[x6]; mov x21,x6; mov x6,x20
str x5,[x21]; mov x5,x21; ands x26,x20,x20; b.ne back
```

W81's audited shape removes six flag sink/polarity instructions and adds two
safepoint instructions: 19 -> 15, a concrete whole-program mechanical reduction
of `887,400,000 * 4 / 348,174,710,727 = 1.019488%`. Seven instructions of the
SVM/FEX mainline gap remain at this PC.

#### `0x402df8`: independent integer width/copy chain

This inner loop already has no state traffic and no terminal flag pack. SVM
spends 28 of 44 mainline instructions on shapes such as:

```text
ubfx x6,x23,#0,#32
mov w22,w6
lsr w12,w6,#0
...
mov w29,w8
lsr w6,w22,#0
...
mov w22,w8
lsr w7,w22,#0
mov w29,w7
```

FEX expresses the same guest body in 20 instructions, with five move/extension
instructions and direct `asr/and/mul/add`. W81 has no materialization to sink
here. The 23 move/width-instruction difference at this single entry corresponds
to a `2.568366%` whole-program mechanical ceiling before correctness and RA
constraints; adjacent matrix rank 6 (`0x402d58`) has the same high-move family.

#### `0x402814`: non-self-edge flags plus one unpinned GPR

SVM loads guest R14W from `[x28,#768]`; level-2 pins stop at R11, so this is the
one top-three ordinary GPR-state access. It then emits eleven instructions to
publish PF/AF/NZCV before choosing between two non-self successors. FEX uses a
compact `eor; lsl; cmp; sub; cfinv; b.ne` representation around the two loads.

The eight-instruction flag gap at this PC alone is a `0.961931%` mechanical
whole-program ceiling, but it is not a W81 self-backedge candidate. Removing it
requires a cross-edge representation/consumer proof, not another local terminal
DCE.

## 5. CoreMark kernel structure

The normal W71 report prints only top 20. To classify all 1,701 executed PCs,
`/private/tmp/w85-gdb.py` stopped at `DumpAtExit`, read the probe's process slots,
and emitted every `(guest_pc, entries, host_static)` tuple. The command was:

```sh
orb -m ubuntu bash -lc '
  gdb -q -batch \
    -ex "set env SVM_RA_HOT_COALESCE /tmp/w85-all/hot.txt" \
    -ex starti -ex "source /private/tmp/w85-gdb.py" -ex continue \
    --args /tmp/w85-build/source/translator/linux/svm_translator_linux \
    /Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64 \
    0x0 0x0 0x66 150000 7 1 2000 > /tmp/w85-all/gdb.txt 2>&1'
```

Raw summary:

```text
[w85-all-entry-summary] versions=1701 executed=1701
```

Grouping was by ELF symbol/range: list `0x402200..0x402a00`, driver
`0x402a00..0x402a80`, matrix `0x402a80..0x403200`, state
`0x403200..0x403840`, and shared CRC `0x403840..0x403a00`.

| family | entry-weighted host instructions | share |
|---|---:|---:|
| list (including compare/list support) | 124,110,280,621 | 35.645978% |
| matrix | 67,753,805,459 | 19.459715% |
| state | 123,438,632,877 | 35.453073% |
| shared CRC | 32,845,203,140 | 9.433541% |
| iterate/driver | 25,500,115 | 0.007324% |
| runtime/other | 1,288,988 | 0.000370% |
| **total** | **348,174,711,200** | **100%** |

The 473-instruction difference from the first aggregate is startup-path noise
between a direct run and the gdb-controlled run (`1.4e-7%`); both passed the
same CRCs, and the difference does not affect the shares.

Representative same-PC mainlines support the weighting result:

| family / PC | SVM | FEX | ratio | dominant excess |
|---|---:|---:|---:|---|
| list `0x402680` | 19 | 8 | 2.38x | self-edge flags, partly W81 |
| list `0x402814` | 24 | 8 | 3.00x | non-self-edge flags + moves |
| matrix `0x402df8` | 44 | 20 | 2.20x | zero-width and tied-copy chain |
| state `0x4033ec` | 19 | 7 | 2.71x | byte-CMP PF/AF/NZCV publish |
| CRC `0x4039b8` | 42 | 22 | 1.91x | width bridges + edge flags |

Therefore list is marginally the largest mechanical family, but state is only
0.193 percentage points behind. Together they are 71.10%; calling CoreMark a
single CRC-kernel problem would be misleading. CRC is the least inflated of the
four representative families, while matrix presents the cleanest independent
optimization mechanism.

## 6. W36 versus post-pin master

W36's CRC-loop account was 59 SVM versus 22 FEX steady instructions, with the
gap assigned as 43.2% flags, 35.1% GPR state access, and 21.6% move/width.
Subsequent W38/W55-W65/W59/W78 work changed that shape materially.

| mechanism | W36 conclusion | current evidence | disposition |
|---|---|---|---|
| GPR state | 35.1% of old CRC-loop gap, 13 accesses/iteration | one ordinary state load in 87 top-three SVM mainline instructions; state-coalesce ceiling 0.077356% | pin wave absorbed the old main account; do not reopen cache/PIN3 |
| flags | 43.2% of old gap | still 15/51 of top-three gap, but concentrated in self-edge and non-self-edge publish shapes | W81 covers only the safe self-edge subset; broader work remains high risk |
| move/width | 21.6% of old gap | 35.102447% whole-program tag; 29/51 of same-PC top-three gap | now the largest independent account |
| spill | not a leading W36 account | exactly zero dynamically | no CoreMark spill project |
| NaN | not relevant | effectively zero | no CoreMark NaN project |

The comparison is directional rather than an identical denominator: W36 was a
single CRC hot loop, whereas W85's 35.10% is a whole-program entry-weighted
mechanical tag. The same-PC table supplies the denominator-controlled proof that
state has disappeared and move/flags now dominate.

## 7. What W81 covers and what needs a new item

### Covered by W81 P1

- Safe self-backedges whose cold exit currently forces the audited flag sink.
- At `0x402680`, net four instructions per taken iteration, or 1.0195% of the
  current whole-program mechanical denominator.
- Applying the same net four to all W79 recurrent-source entries gives only an
  **upper envelope** of `4.3009%`; actual eligibility and W81 codegen must be
  remeasured. This is not a promised speedup.

### Independent new project: integer width/copy canonicalization

Start audit-first at `0x402df8` and the adjacent matrix loops.

- Target repeated `ubfx #0,#32`, `lsr #0`, W/X transfers, and physical copies
  around 32-bit typed operations.
- Use RA's authoritative last-use data and existing tied mechanisms; W70/W74
  already showed that approximate last-use is unacceptable.
- Gate on a dynamic deletable subset, not the broad 35.10% mnemonic tag. The
  one proven same-PC block exposes a 2.5684% whole-program mechanical ceiling,
  enough to justify the audit.
- Default OFF for a spike; require no increase in spill, high-water, scratch
  escalation, helper snapshots, or host bytes outside candidate units.

### Possible later project: compact cross-edge flags representation

`0x402814` and state `0x4033ec` show that non-self edges still pay much more than
FEX. This is not the previously rejected P5 lazy-source proposal: W67 measured
PF/AF forced-materialization lower bounds near 98%. A viable project would have
to change the carried representation and teach edge consumers/fault recovery to
understand it. That crosses W30 fault/helper/direct-link contracts and should
wait for W81's safepoint/fault infrastructure plus a fresh frequency audit.

### Not projects from this data

- PIN_EXT=3 or unit-local GPR cache: post-pin state is no longer a leading gap,
  and W60/W72 already measured the allocator/boundary regressions.
- Spill robustness as performance work: CoreMark recorded zero dynamic spills.
- NaN optimization: only 14 startup instructions in a 348-billion-instruction
  mechanical denominator.
- BlockLink/direct dispatch: the hot differences are inside linked mainlines,
  consistent with W36's already-small BlockLink account.

## 8. Repository discipline

No source, test, golden, linker, or benchmark-harness file was changed. The only
worktree addition is this report: `docs/w85-coremark-reattr.md`.
