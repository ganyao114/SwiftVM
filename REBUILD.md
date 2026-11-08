# SwiftVM reconstruction

Repository integration: this measured tree is committed on
`refactor/middle-pipeline-rebuild-parity`. The frozen original workspace and its
WIP remain in place. Historical paths and "no commit" statements below describe
the measurement checkpoints before integration. Portable build instructions and
the retained evidence subset are in [README.md](README.md) and the
[checkpoint](docs/codegen-parity-checkpoint-2026-11-04.md).

The current indirect dispatcher uses a guest-thread ready L1 and the VM/domain
CodeCache as shared L2. The old per-site arrays and `callable_directory` option
have been retired. See the [implementation and measurements](docs/thread-dispatch-cache-2026-11-08.md)
for the layout, invalidation contract, compile guard and retained performance limits.
The latest FP and compile-resource checkpoint is recorded in
[FP status optimization](docs/fp-status-optimization-2026-11-08.md) and
[dispatch resource optimization](docs/dispatch-resource-optimization-2026-11-08.md).
Thread L1 is now provisioned before publishing actual dynamic exits; static-only
JIT code can use L2 without mapping L1. The SMC miss view is created before the
first write watch. Persisted code is provisioned conservatively on restore.
The retained measurements include remaining FP and x87 Block compile gaps.
The batch descriptions below retain their historical configuration.

Latest tested parity checkpoint: [implementation progress](docs/codegen-parity-progress-2026-11-03.md).
The latest retained full checkpoint is batch 37. Both scopes pass this fixed
input's quantified performance goal: runtime, cycles and instructions <=1.10x
FEX, and cold compile excluding byte parsing <=2x. Compile is
48.187132 / 84.762869 ms for Block / requested Function versus FEX 47.3196875 ms
(1.018331577x / 1.791281251x). Valid runtime is 16.302 / 15.493 s versus
FEX 15.072 s (1.081608280x / 1.027932590x); cycles are 1.081087685x / 1.030169884x,
and user instructions are 1.027346518x / 0.986518968x. This acceptance covers the
original Bionic/CoreMark input and recorded device/configuration; full ISA
and all-application performance are outside that evidence. Formatter figures
below remain the original baseline.

The active tree is `/Volumes/Data2/SwiftVM-rebuild-20261102`. The original
`/Volumes/Data2/SwiftVM` source, Git/index and WIP remain frozen. This build
links the new middle pipeline; it does not wrap or link the retired pipeline.

The rebuilt path runs the original static x86-64 Android Bionic/CoreMark ELF
on RedMagic NX729J. Block, requested Function and fixed FEX complete the same
input and arguments with matching CRCs. The final CPU4 run uses 100000
iterations; all three exceed 10 seconds and pass CoreMark validation.

The following table is the historical formatter starting baseline, recorded
before the parity batches.

| Measurement | Block | Requested Function | Fixed FEX |
| --- | ---: | ---: | ---: |
| Cold compile excluding actual byte parser, median of 3 fresh processes | 37.163 ms | 67.080 ms | 47.231 ms |
| Compile / FEX | 0.787x | 1.420x | 1x |
| Guest runtime, 100000 iterations | 36.943 s | 29.236 s | 15.093 s |
| Runtime / FEX | 2.448x | 1.937x | 1x |
| Whole child user instructions / FEX | 2.754x | 2.075x | 1x |

The compile comparison charges formation, materialization, IR, optimization,
allocation, encoding, publication, failed attempts, retries and release. Only
calls into the byte parser are removed. These per-byte timing probes are
intrusive. The long run uses the ordinary runner with PMU stat, without byte
clocks, dispatch counters or sampling; the actual workload PIDs were checked
with `Cpus_allowed_list=4`. All modes use the same guest, configuration and
CPU4 frequency limits; no frequency was locked or written.

Function has 24 local Block retries in the 20-iteration compile comparison;
722 Function attempts plus 24 Block attempts are charged. It is not a
zero-fallback Function result. The latest batch37 meets the <=2x total compile
target and the fixed-input1.10x runtime/cycles/instructions margin. Implementation
of the [FEX parity plan](docs/codegen-parity-plan-2026-11-03.md) has reached
batch37; both ordinary runtimes remain slower than FEX within that margin.

```text
source/runtime/ir/              # 24B operation / 24B block / 8B word; sole owner
source/runtime/frontend/x86/    # decoder buffer, formation, neutral IR birth
source/runtime/compile/         # common in-place optimization; one u64/node table
source/runtime/backend/arm64/   # actual homes, direct encoding, private helper-call ABI
source/runtime/execution/       # state/mappings, slabs, one PC directory, native execution
source/launcher/linux/main.cpp  # CLI and coarse diagnostic output
benchmarks/app_inputs/          # supporting official CoreMark syscall-port recipe
```

IR has no use lists or per-node allocation. Submit is an O(1) owner move.
The optimizer makes three walks over that same storage. Block uses its light
allocation path; Function builds actual RPO and loop lifetime closure. One
16B Home table feeds the code writer, without plans/actions/certificates or MIR.
The current file responsibilities and contracts are in
[compile-framework-contract-2026-11-02.md](docs/compile-framework-contract-2026-11-02.md).

Known-target input permits neutral shifted-address/conditional-store forms.
With the execution service's explicit private-state contract, fixed 16-slot
GPR and XMM caches avoid redundant births before IR exists. Different GPR views,
partial writes and real observation points publish their values. Unknown and
EarlyOff retain generic input; the frontend includes no ARM64 backend headers.

Native units retain sixteen guest GPRs and sixteen XMMs in pins. On FEAT_FlagM
hosts, NZCV stays in hardware, raw PF in x1 and the AF input xor in W29;
reserved packed bits remain in the state shadow. Block-local PF/carry recipes
defer a limited set of carrier updates. Other hosts use packed flags. LRCPC2
addresses consume their actual signed imm9. Eligible ordered accesses use one
hot instruction and a three-word cold SIGBUS fallback, with the original W^X
patch service. Partial XMM aliases and real cold/helper/fatal boundaries
synchronize the complete affected vector state.
Only cold misses and real services enter C++; warm links use the shared ABI and
one PC directory. Code/file write aliases revoke links before safe retirement.

Scalar-double consumers retain raw U32 MXCSR at GuestState+416. Root, cold,
helper and fatal/normal exit boundaries isolate host FPCR/FPSR. LDMXCSR really
exits and re-enters after a mode update. AFP maps independent controls directly;
without AFP, scalar arithmetic and packed comparison consume the actual raw
DAZ/FTZ controls with small bit operations. The device reports AFP=0, and this
path was exercised. Numerical conversion sentinels, NaNs, unchanged upper lanes
and manual MXCSR status round trips are preserved. Automatic MXCSR sticky
accumulation and IEEE traps remain outside the frozen original FP contract.

```sh
./svm_rebuild_linux --scope block guest.elf [guest args...]
./svm_rebuild_linux --scope function guest.elf [guest args...]
./svm_rebuild_linux --decode-timing --scope function guest.elf [guest args...]
```

This remains a single-thread static-ELF runtime with the implemented x86-64
subset. Dynamic ELF, full ISA coverage, complete guest Linux signal delivery
and external file writers/unregistered aliases remain open. Unsupported
syscalls return ENOSYS; the measured Bionic handles six signal-setup failures
(131/14/13). They are not faked successes. This checkpoint does not claim full
legacy-VM feature parity.

The original formatter baseline source snapshot, exact ELF identities,
commands, raw results, PMU counts and live CPU masks are retained in
`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-direct-emit-20261103/device-formatter`.
The original guest SHA is `d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba`;
baseline runner SHA is `3e32e04a97ad3635e2a288aa874fd3ed344c5db4443a5127141bacf98fe9bc1e`.
Its source-as-built digest is `0aaf165c7df7c9c099b499ed520ed719629b3140e6703951b1f054cd94a6e8a3`.

The retained historical batch 34 evidence is in
`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-parity-implementation-20261103/batch34`.
Its 80-file source-as-built digest is `c176c015f5d5ed0d79f0ed7b478def0869a40e93c66971ee324280c150a8eb58`;
runner SHA is `365511fea7fa0fd58a1d9ce63f3cb7b5d81206cad6410fcd11e5c632a4453b19`
(8,420,272-byte unstripped Android ELF).

Batch34 supplies `callable_directory=true` only from the actual runtime
owner: slots/mask stay valid and every slot has a callable native/cold entry.
Default public contexts retain their guards. Inline first-slot ready dispatch
falls from ten instructions to eight by removing CBZ slots and CBZ entry.
Mismatches still inspect occupied; the prior entry shortcut is removed too,
so each additional ready collision's local loop grows from eight to nine.
This is a measured tradeoff in the same directory, not two saved instructions
per probe or a new cache, IR, pass or register bank.

Only three actual units were generated offline, with the new option printed
as true and Known/a64/features63. B21c3b3 has the same IR/Home as its31 artifact,
33 to29 words; Fn21c483 matches32 IR/Home,158 to148 words. B21c4e5 has no old
same-PC artifact and is reported only at its actual32 words. All three have
frame/spill0; the existing raw-flags arithmetic and fault recipes are retained,
with real hot/cold positions recalculated. The short report is
`batch34/native-quality-review.md`. It is not a new RX binding, and full words
are not multiplied by entry counts.

The32 CBNZ self head and33 conditional primary near-head links remain: the
resolved2197e0 nonzero self path is six instructions, and a selected near
primary conditional edge uses one branch instead of two. Unselected/far/cold
paths retain their real gate, and offline LDR/BR is not called the warm path.
Complete32/33 histories and separate FEX denominators remain in progress.

Ordinary34 whole-child user counts are cycles
19,386,747,792 /18,482,725,400 /FEX17,353,332,914 and instructions
67,029,624,149 /64,392,931,982 /FEX61,981,529,713. Ratios are
1.117177195x /1.065082166x and1.081445141x /1.038905175x respectively.
Branch misses are43,606,343 /13,836,358 /FEX18,471,891. Relative to33, each
scope's own elapsed time falls1.407%/2.371%; FEX changes too. Whole-batch
results are not split into hunk-specific gains. Function meets this cohort's
1.10x time/cycles margin; Block remains above it, so the total goal is open.
Spill slots25/42 and maximum frames48/64 bytes remain unresolved.

All three cold Block records have2045 attempts, N36672, words65867 and native
bytes318516. Function retains746 attempts (722 Function plus24 Block retries),
N86893, words152552 and native bytes513740. Every actual failure/retry,
formation, IR, allocation, encoding, publication and release remains charged;
only byte-library calls are subtracted. The ordinary long run has no byte
clocks or diagnostic counters. No diagnostic timing replaces ordinary QoR.

Host five contracts, Android264478 accumulated fixture checks and the original
12/4170 pipeline, formatter, SMC, EOF and fault/state checks pass. This is not
264K distinct semantic cases. Earlier24/27 fixture failures and30's test-only
ADL failure remain archived; no source-as-built identity is relabelled.
The34 phone namespace was removed: du37,749,760 bytes, df free space increased
37,810,176 bytes, remaining0, with no active processes, open files or errors.
Documentation before/after/diff/hashes are separate in
`batch34/post-measurement-docs`; archived production34 and its80-file identity stay frozen.

Batch36 retains Uniform IR, the three optimizer walks and the original Home,
fault and link owners. C removes the byte XOR result cut only when its sole
numeric use is the adjacent pinned-byte store; SXTB into PF1 and BFXIL each
consume their own low eight bits. Full numeric/raw/multiple-use paths keep
their canonical results. A dead full-width self TEST uses SUBS into PF1 with
explicit carry polarity; the existing deferred self-loop path is unchanged.
B folds an exclusive same-block default-metadata U64 Add/Sub address into
the load's existing neutral Expression during the original forward walk;
the original final DCE removes that producer. There is no additional pass,
node table or IR opcode, and the oldW+2N edit cap remains.

The batch36 evidence is in
`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-parity-implementation-20261103/batch36`.
Its 80-file source-as-built digest is `87d1c870bf05ea0d69f2dcbb0ff25432d6ab89c185781abff737ac89d14b09d0`;
runner SHA is `14de0114e2921a67388f098adeca46ca398d799fffff312c24ba1c72c7329739`
(8,436,728-byte unstripped Android ELF). Ordinary user cycles are
19,034,878,099 /18,241,695,053 /FEX17,235,586,414; instructions are
64,648,262,642 /62,117,779,015 /FEX61,981,473,747; branch misses are
44,263,899 /18,257,864 /FEX18,754,341. Function meets the runtime/cycles
margin; Block remains above 1.10x. Whole-batch results do not identify each
hunk's contribution. Spill slots25/42 and maximum frames48/64 bytes remain.

All three cold Block records have2045 attempts, N36672, words65867 and native
bytes317268. Function still charges722 Function plus24 Block retries,
N86893, words152552 and native bytes509492. Host five contracts,
Android264542 accumulated fixture checks, original12/4170 pipeline,
formatter/SMC and expected EOF exit125 pass; the accumulated Need count is
not a count of distinct semantic cases. Four bounded offline units retain
their actual fault/link contracts; no new RX binding or words-times-hits
runtime estimate is claimed. See `batch36/native-quality-review.md`.
The phone namespace cleanup removed37,831,680 bytes, increased df free space
by37,892,096 bytes, and left no files, active processes, open files or errors.
The three post-measurement documents are archived separately in
`batch36/post-measurement-docs`; they do not change the as-built80 identity.

Batch37 adds three bounded C selections within the existing storage and ABI:
an adjacent sole-use unflagged U32 multiply/add becomes MADD in the consumer's
actual Home; the existing sole pinned-byte-store check also permits an
unflagged U8 immediate Add/Sub to omit its result cut; and the strict signed
ordered-load pair additionally accepts U32 to U64 via LDAPURSW/LDURSW.
Metadata/legacy, actual views, spills, PF conflicts, fault PCs and publication
boundaries retain their original fallbacks. The existing single consumer ID
is reused; there is no new IR, table, pass, pin bank or flags representation.

Batch37 ordinary user cycles are18,728,836,155 /17,846,732,735 /
FEX17,324,067,619; instructions63,676,531,817 /61,145,976,904 /
FEX61,981,552,180; branch misses43,130,249 /14,228,779 /FEX18,729,668.
The measured goal passes for both scopes. Spill25/42 and maximum frames48/64B
remain, and the unimplemented20B first-eviction proposal is not an acceptance
prerequisite. Cold Block still has2045 attempts/N36672/words65867/native317132;
requested Function charges746 attempts=722 Function+24 Block retries,
N86893/words152552/native509124. Existing Host5, Android264579 accumulated
Need/fixture checks,12+4170 pipeline and formatter/SMC/EOF125 checks pass.

Evidence is retained under
`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-parity-implementation-20261103/batch37`.
The explicit fixed-input decision is in [measurement-receipt.json](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-parity-implementation-20261103/batch37/measurement-receipt.json).
It evaluates all eight gates; the original runtime-only JSON retains its
historical hard-coded false placeholder unchanged. Source-as-built80 digest
is `79c8bf0c2046ab35ece932b5e74310b13d5de339b00ee01e4def117235cdd985`;
runner SHA `d927b24230bd4e9b80cb771394d8761dd1c382a59a0391f750321d9a9aa051aa`
(8,447,952B). Four offline units confirm the intended selections without a
new RX binding or hunk-specific dynamic attribution. CPU4 limits614400–1171200kHz
and temperature reading254 are unchanged before/after. Cleanup removed37,889,024B,
increased df free space37,949,440B and left remaining0 with no active/open/errors.
The three documentation updates are archived in `batch37/post-measurement-docs`;
their post-measurement hashes do not relabel the as-built80. Original source,
Git/index/WIP remain frozen; this work made no commit or push.

Earlier generated-code comparison: [codegen-quality-comparison-2026-11-03.md](docs/codegen-quality-comparison-2026-11-03.md). The historical comparison adds no production changes.

独立目录目标计数已闭合：两scope实际native ready-hit约612M，状态机目标占74.77%；仅用于定位，未覆盖静态链接或替代普通性能结果。该隔离诊断以冻结28为base，当前已测checkpoint见顶部37。原始证据见`batch28/directory-target-census`。
