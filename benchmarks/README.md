# IR construction, optimization, scalar compilation and budget checks

For the frozen 6272-byte bench-suite ELF, `app_inputs/split_bench_suite.py`
creates a separate RX-code/RW-BSS input without changing instruction bytes,
constant bytes, guest PCs or data addresses:

```sh
python3 benchmarks/app_inputs/split_bench_suite.py \
  --guest /path/to/frozen/bench_suite_x86_64 \
  --output /path/on/data-volume/bench-suite-rx-rw
```

The generated JSON records both identities and segment layouts. Use this as a
separate formation diagnostic: the original RWX guest requires SVM's watched
single-block policy even under Function allocation. Retain its original results;
the RX/RW variant does not demonstrate faster handling of writable code.

The current application checkpoint is contract section 28 and `REBUILD.md`.
It includes native unit linking, real fault-PC recovery, sixteen pinned guest
GPRs and native NZCV on actual FEAT_FlagM hosts (packed fallback otherwise).
`--decode-timing` also selects a counted native dispatcher; ordinary runs
emit `null` for dispatch/cache-hit counters and execute no counter writes.
Resolved links bypass that diagnostic lookup count. Keep implementation checks
focused; run full QoR/PMU acceptance after feature convergence.

The benchmarks execute the rebuilt IR and explicit optional optimization/scalar
compilation paths. Synthetic emission and the
limited MOV/LEA/control x86 byte fixture are not application workloads, complete
compilation, FEX comparisons or final performance acceptance. No old VM, pass,
IR analysis, allocator or backend runs; the scalar compiler uses the new backend.

```sh
./svm_ir_static_build_submit_bench --nodes 24,128 --blocks 1 --requests 32 --samples 5 --warmup 1
./svm_x86_build_submit_bench --requests 32 --samples 5 --warmup 1
./svm_ir_static_build_submit_bench --nodes 1024 --requests 32 --mode batch-diagnostic
```

The default mode is `sequential`. Request count defaults to a fixed 32 positive
requests, set by `--requests`; neither count nor batch size is calibrated from
candidate speed, memory usage or allocator state. Each timed positive request
constructs, submits and releases its Program and Builder before the next request.
The primary sequence timer includes real destruction and fixed negative requests.
`ns_per_attempt` divides by **all** positive and negative attempts, not successful
requests alone. Large synthetic sizes can take substantial time with 32 requests;
choose a fixed count explicitly for both versions rather than auto-calibrating it.

Synthetic sequences append two fixed negative requests: a successful LoadImm
prefix followed by InvalidOpcode, and a successful LoadImm prefix followed by a
foreign-unit Value rejection. The timed requests then abandon and destroy their
Builders. Their allocation, partial-build, rejection and cleanup costs are inside
the primary timer; expected rejection does not make the benchmark process fail.
The x86 schema-4 sequence appends three Unsupported requests: first UD2,
MOVabs then UD2, and MOV CR0,RAX rejected after its source read already emitted
IR. Explicit untimed owner snapshots measure their N/E/words as [0,0,0],
[2,1,4], and [1,0,1]. All have one declared block. Timed requests collect no
snapshot and include real partial construction, rejection and release. Accepted
byte/instruction counters cannot replace abandoned IR counts.

Before warmup and timed samples, an independent **untimed replay checks every
request in the same fixed sequence**. Every successful Program is checksummed;
synthetic rejected prefixes are recovered and submitted only in this verification
pass to check rollback. The default timed requests do not traverse their payloads
or pause before destruction to checksum them. They count actual requests, N and
operand-arena words with O(1) readers; timed E and abandoned-prefix words are
explicitly from the verified fixed recipe. The output does not claim that each
timed Program itself was checksummed. Request accounting and, in the synthetic
benchmark, global-new tracking overhead are included in the primary timer.

`batch-diagnostic` explicitly retains simultaneous live requests and separate
construction/Submit/destruction probes; it has **no primary latency**. It uses
exactly `--requests` slots and verifies every existing slot outside its timers.
Fixed negative-request cost is a separate diagnostic component. Payload checking
changes destruction cache state; these probe results are not ordinary sequential
latency and must not replace it. No automatic batch calibration remains.

Synthetic global new/new[] tracking includes aligned new. It reports requested
allocation bytes and peak live bytes, excluding tracker/allocator headers and
arbitrary malloc; these are not RSS. Allocation phases 0/1/2 mean construction,
Submit and destruction. Submit allocating, an unexpected request outcome or live
tracked bytes remaining after cleanup fails the executable. The x86 benchmark
has no allocation tracker and cannot claim measured Submit-zero allocation.
Submit nanoseconds in diagnostic mode are near timer/loop resolution; zero
allocation is not proof of zero payload access. Fresh empty vectors, calibration
being absent and fixed counts do not mean cold process/OS/allocator state.

Synthetic JSON schema 3 and x86 schema 4 are distinct from retained historical results. The x86 failure sequence changed; schema-3 x86 results cannot be relabelled as schema 4. Its primary
metric is whole-sequence build/Submit/release time; old per-program construction
medians must not be relabelled as this metric. All attempts, failures, abandoned
prefixes, actual N/arena-word totals, verified E, warmup and verification counts
are identified. The unchanged positive synthetic recipe still covers two scopes,
three metadata modes and growth/exact storage. Growth is primary; exact Reserve
is only a capacity diagnostic and uses the format's actual storage units.

`canonical_semantic_v1` includes scope/target metadata, opcode/result/block,
operation metadata, complete logical payload/type views, compound boundaries and
terminal IDs/views. It ignores Identity(1) wrappers, layout offsets and unit
cookies. It is a diagnostic hash, not formal equivalence proof. `logical_cells`
are canonical cells, E is Value occurrences plus terminal Value occurrences, and
physical words are raw 8-byte operand-arena words. The arena may contain retired
payload after edits; semantic/E readers visit Operations and each owner-local
Operands range, never interpret the entire arena as logical cells. The current
append-only fixture has no retired ranges. Wide payload words are not uses.
`SVM_IR_BASELINE_COMPACT` reads the current comparison baseline (24/24/8 layout,
FlatOperands().WordCount and actual-operation range). `SVM_IR_BASELINE_LAYOUT`
retains the older 16B-cell field adapter only for historical diagnostics;
new and baseline synthetic inputs stay identical. Static/dynamic writers still
share the actual core storage; code-size costs remain part of comparison.

The v1 recipes and replay format have a fixed implicit entry0. Their current
untimed qualification explicitly rejects nonzero entry, preserving the historical
hash identity rather than pretending it represents arbitrary CFG entry. The
Function benchmark below checks the actual entry separately with work and code.

## Fail-closed budget gate

```sh
python3 benchmarks/check_compile_budget.py benchmarks/compile_budget.template.json --stage A
python3 benchmarks/check_compile_budget.py benchmarks/compile_budget.template.json --stage full
python3 benchmarks/check_compile_budget.py --self-test
```

The schema-3 template deliberately contains null evidence. Missing real inputs
return `MISSING`/exit 2. Candidate **and FEX** must explicitly identify an
application workload, `synthetic:false` and `partial_compile_coverage:false`; both also require
`measurement_kind:"application_compile"`. An application-derived
`birth_ir_writer_replay` is still a writer replay, so it cannot pass A or full.
Missing eligibility fields also reject. Fixed policy is A=0.50, B=0.25, C=0.90, D=0.25 times the same-round
fresh fixed FEX reference with decode excluded on both sides, plus other=0.10 and total<=2.00. These are design
allowances, not measurements. Input policy overrides reject. Decisions compare
integer nanoseconds against fixed integer percentages, without float tolerance.

The current comparison basis is `compile_excluding_decode`, required explicitly
in the manifest and both results. A/full are spans within that basis. Local byte
decoder completeness and speed are deferred; complete post-decode IR work still
must be measured. A trusted supplied decoded input is allowed without waiting
for a complete new byte decoder. The counted work includes IR storage/reset,
construction/Submit, local IR edits, optimization, RA, encoding, publication,
failed/abandoned work and release, wherever those operations occur.

Charged `wall_intervals_ns` must already exclude pure decode. Each result also
supplies `decode_exclusion_kind:byte_decode_only` and
`excluded_decode_intervals_ns`, on the same wall axis. The lists must be disjoint;
`[]` explicitly means this measured path uses supplied decoded input. Never
exclude a whole frontend function that also constructs or resets IR. Do not
subtract a guessed decoder percentage from historical compile totals or compare
SVM without decode to FEX including it. The full success flag is now named
`compile_excluding_decode_2x_cost_gate_passed`.

The manifest's `wall_clock` is `{kind:"monotonic_wall",unit:"ns",domain:...}`.
Every result must name that same specification. All request/thread intervals
within one result share **one wall origin**; per-thread CPU clocks or separate
thread-relative origins cannot be combined as a wall axis. A result supplies
`wall_intervals_ns:[[begin,end],...]`: integer ns, 0<=begin<=end. Its complete
cost is the length of their union, allowing multiple request windows and
concurrent/nested windows without adding the same wall time twice. FEX must
cover every compile thread and have `measurement_span:"full"`. Complete
candidate and FEX windows must both have positive wall duration.

Candidate `measurement_span` is `A` or `full`. Each required phase supplies its
named `endpoints` and `wall_intervals_ns` on the same axis. Phase intervals must
be wholly contained in the complete window, including across disjoint windows.
Raw intervals within one phase are unioned. In full mode the A/B/C/D unions must
be mutually exclusive; touching endpoints are valid, actual cross-phase overlap
returns MISSING even if an old `intervals_nonoverlapping` boolean says true.
`other` is complete-window union minus covered-stage union. Actual parallel
work in different phases needs a defined exclusive wall attribution before
qualification; CPU sums or an asserted boolean cannot substitute for it.

Lengths are derived from actual intervals. Optional `inclusive_ms` and legacy
`inclusive_compile_ms`, if present, must exactly agree with that record's union
(full ns precision; no averaged or rounded replacement). A genuinely zero-work
phase may have `wall_intervals_ns:[]` and explicit `inclusive_ms:0`; an absent
phase is not zero work. Never manufacture intervals from an old accumulated
counter. Missing raw bounds remain missing.

`--stage A` only reads phase A, so B/C/D need not exist. An A-span record must
charge its **entire** complete window to A. A full-span record can also be
checked for A alone, but still must supply its positive complete wall window.
A success returns `PASS_A_BUDGET_ONLY`, not full 2x or quality acceptance.
Default `--stage full` requires full-span data and all four phases, checks each
allowance, other<=0.10 FEX and total<=2.00 FEX. Overspending returns
`OVER_BUDGET`/exit 1. A full PASS grants only the compile-cost gate; quality is
independent and `quality_acceptance` remains false.

Other evidence is unchanged: fixed application/configuration hash, both
scope-specific complete attempt ledgers (case_scope/id/outcome/abandoned/
accounted/N/E/words, including failed prefixes), actual device/affinity/policy
identity, empty compile caches, same-round fixed candidate/FEX source and binary
identities, named reference/stage endpoints and complete FEX thread coverage.
Candidate totals must equal the complete ledger. Sequence SHA is SHA256 of its
sorted-key compact JSON. Result artifacts are loaded by manifest-relative path
and verified SHA256. Unknown prefix counts or uncharged failures stay missing.

`--self-test` uses only fictitious in-memory schema records. It covers A-only
and full admission, both sides' ineligible workload flags, the former overlap
counterexample, out-of-window phases, inconsistent durations, NaN/missing data,
wall union versus CPU accumulation, explicit zero-work phases and overspending.
It is **not a performance measurement or a real budget PASS**. Old evidence and
benchmark results are not converted or relabelled to satisfy this schema.


## Application birth-body writer replay

`normalize_birth_trace.py` reads complete `SVM_BIRTH_TRACE_FILE` raw events from
the separately preserved frozen-frontend diagnostic. It does not use formatted
post-optimization dumps. Raw remains the primary evidence: physical compound
slots and shift, complete immediates/views/flags, eight helper-trait bytes and
source effect dictionaries, every birth, later edit and recursive terminal.
The normalizer rejects missing/corrupt records and unmapped payloads, rather than
filtering requests. Its receipt binds raw, normalizer/schema and output hashes.

```sh
python3 benchmarks/normalize_birth_trace.py --help
./svm_ir_birth_replay_bench --input block.svmir --writer static --samples 5 --warmup 1
./svm_ir_birth_replay_bench --input function.svmir --writer generic --samples 5 --warmup 1
```

The first line is `SVMIR_REPLAY_1 BODY_ONLY` or `SVMIR_REPLAY_1 FULL_TERMINALS`.
Q begins an actual IR-owner request; C/B declare/begin a block, I plus V rows
supply an actual instruction and flat atoms, T ends a block, P submits, X
abandons. Opcode/type names come from the real declaration. Result type is the
actual natural type: Arg0 is a default and an explicit result may retain a wider
source view. Expression code retains operation in its low byte and shift in its
high byte; helper traits use all eight bytes and source effect ranges stay in
the bound external report. They are not a newly imported runtime registry.

BODY_ONLY is explicit component measurement. Old complex terminal trees are
preserved in the report and replayed as Unreachable. All births remain in original order within each owner,
including later retired births. A revisited source block gets another replay
segment; original blocks, extra segments and mappings are listed separately.
Later payload edits/unlink/retire remain in raw/report and their cost is outside
this measurement. Metadata-only edits may be folded and are counted in the
report. Multiple actual owners split a top-level source request with the full
mapping retained; backend failures remain X and Block fallback remains Block.
FULL_TERMINALS rejects semantics or edits that cannot be represented faithfully.

All requests get an untimed qualification replay. Published bodies compare their
actual stored payload/metadata/type/view/terminal with the normalized recipe.
Abandoned requests check actual birth results and CountWork; their stored payload
is not individually canonicalized and no fake terminal is added to make them
submit. Timed samples call neither CountWork nor payload hashing. They include
unit patching, runtime opcode dispatch into declaration-generated Emit (or
Append), construction, Submit/abandon, release and per-request timer overhead.
Parsing and receipt/hash audits are outside timing. No allocation tracker runs.

Default writer-only output is JSON Lines per actual scope,
`measurement_kind:birth_ir_writer_replay` and `grants_stage_a_full_or_fex:false`.
A real source application does not turn a body replay into actual decode,
formation, optimization, allocation, encoding or publication. Pair same-sample
scope totals before taking a median for a mixed-mode capture; do not add separate
medians. Both current application input sets, their limits and Android results
are in `phase-a-performance-closure/measure-summary.md` outside this tree.

## Common optimizer and actual scalar compiler

```sh
./svm_ir_birth_replay_bench --input block.svmir --pipeline optimize --samples 5 --warmup 1
./svm_ir_compile_bench --requests 128 --nodes 256 --samples 5 --warmup 1
./svm_ir_compile_bench --requests 128 --nodes 1024 --samples 5 --warmup 1
./svm_ir_compile_bench --requests 128 --nodes 256 --flags on --samples 5 --warmup 1
```

`--pipeline optimize` preserves the original writer qualification and executes
the real in-place optimizer for every published P. X still builds and abandons
the complete original prefix, without a fabricated terminal or optimizer call.
The primary endpoint includes build, Submit, optimizer and release; it does not
clock each helper or subtract timer probes. The untimed qualification checks
actual optimized references, live terminal inputs and stable owner/node identity,
then records an output mix. This is structural checking, not a semantic proof.
The output kind is `birth_ir_writer_optimize_replay`, with
`grants_stage_a_full_or_fex:false`. Original birth counts include all outcomes;
optimized live-node/arena counters cover only published outputs. All retired
births remain in the normalized Program; BODY_ONLY projection and omitted old
edits therefore prevent this from representing actual final-program B work.

`ir_compile_bench` builds state-dependent scalar IR, optimizes the same owner,
chooses actual Block/Function homes, emits native ARM64 words and destroys both
owners before the next request. Thirty-two simultaneously live values force real
spills. The `--nodes` option sets arithmetic work; actual N/E/word/block totals
come from a separate untimed qualification and are printed explicitly. Whole
sequence wall time uses two timestamps, fixed request counts, one warmup and
five samples by default, with scope order alternating across rounds. There is
no CountWork/payload hashing or global-new tracker inside timing. Output is
`scalar_ir_compile_sequence`, explicitly synthetic and excludes publication.
It exercises the real scalar backend, not the full ISA, runtime or FEX gate.

`--flags off` is the default and preserves the original N=320/1088 recipes.
`--flags on` declares All outputs on each arithmetic operation and periodically
feeds a real CondSet through Select into the numeric chain. The actual N becomes
352/1216; `flags_enabled` and output words make that different work explicit.
It enables the explicit backend flags slot but adds no per-node timing or
allocation tracking. Separate compile-time recipes avoid a flags-mode branch
inside every emitted operation. This is a deliberately dense component input;
the x86 frontend's pending-producer optimization normally removes overwritten
flags declarations before Submit. Do not divide timings from these two recipes
as if their N/code work were identical, or use either as a full FEX comparison.

Current Android component evidence, exact hashes, CPU policy, ABBA raw samples,
native execution checks and namespace cleanup are in
`phase-b-c-implementation-20261103/android`. The cost basis remains
`compile_excluding_decode` for future actual application/FEX qualification.
The integer/flags/Jcc increment and same-round flags-off/on measurements are
retained in `phase-c-integer-20261103/android` with their own source/binary hashes.

## Automatic Function region and provided decoded input

```sh
./svm_function_compile_bench --blocks 32 --requests 128 --samples 5 --warmup 1
./svm_function_compile_bench --blocks 128 --requests 128 --samples 5 --warmup 1
```

The byte fixture has separated loop blocks, unreachable INT3 padding and an
entry above the loop body. Direct edges discover actual slices of one decoded
buffer; EntryBlock must remain nonzero. --blocks selects loop blocks; total
guest blocks also include the entry and exit. Actual N/E/word/IR-block totals
are printed, without assuming one file or guest block equals an optimizer pass.

Both endpoints produce identical machine words, actual entry and work counts.
automatic_region includes byte parsing, discovery, buffer materialization and
release in every request. provided_decoded borrows the already prepared actual
buffer; its setup is outside timing, then emission/optimization/allocation/
encoding/release run for real. Publication is absent in both. Do not call the
automatic endpoint decode-excluded, or subtract the endpoints and label the
difference pure decode or pure formation. Full qualification must still charge
input preparation when it occurs inside the real request.

Every fixed request is checked in a separate untimed replay; the primary timer
uses two timestamps around each fixed whole sequence, alternating endpoint
order across rounds. No adaptive counts or per-instruction timing is used.
Output function_region_compile_sequence is explicitly synthetic/partial-ISA
and grants_stage_a_full_or_fex:false. Device results and namespace closure are
in function-discovery-20261103/android.

## Actual static-ELF application

`app_inputs/build_coremark.py` builds the unchanged official CoreMark core at
ordinary O2 with a Linux syscall port. It retains the generated SIMD code and
records exact source/compiler/guest identities. Supply its guest ELF to the
Android `svm_rebuild_linux --scope block|function` application service; this is
not an IR recipe or a borrowed-buffer replay.

`--decode-timing` is explicit and intrusive. Only calls into the byte decoder
are timed/excluded, including unsuccessful parser attempts. Formation, buffer
management, IR work, failed compile attempts, allocation/encoding, publication,
cache insertion and code/cache release stay charged. Off means no byte-decoder
clock reads and null decode-excluded fields. Existing result counters are O(1)
observations; rejected IR prefixes are not claimed in returned-owner totals.

Final acceptance uses the original 1,921,016-byte Android Bionic/CoreMark ELF,
SHA256 d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba,
rather than substituting this syscall port. Its full output/exit and CRCs pass
in both SVM modes and fixed FEX. The matching FEX probe wraps only
DecodeInstructionImpl and retains outer formation. Three fresh-process medians
excluding that parser are 37.163/67.080 ms (SVM Block/requested Function) and
47.231 ms (FEX), or 0.787x/1.420x. Function's 24 local Block retries are included.

The same original guest at 100000 iterations gives 36.943/29.236/15.093 s;
all three pass CoreMark validation and the >=10s duration. PMU stat counts whole
child user events, without sampling or per-instruction timing. Actual guest PIDs
were checked at CPU4. SVM/FEX instruction ratios are 2.754x/2.075x, runtime
ratios 2.448x/1.937x. The earlier inherited-affinity PMU attempt is retained
separately and is not the final timing source.

Exact sources/ELFs, fixed FEX/TSO configuration, raw commands, three-sample
compile data and CPU masks are in qor-direct-emit-20261103/device-formatter.
The final source-as-built digest is 0aaf165c7df7c9c099b499ed520ed719629b3140e6703951b1f054cd94a6e8a3;
runner SHA is 3e32e04a97ad3635e2a288aa874fd3ed344c5db4443a5127141bacf98fe9bc1e.
Earlier evidence retains its original checkpoint scope. Component allocation
budgets remain useful attribution targets; total <=2x and concentrated final
quality are the user acceptance targets, without inventing a new 2x runtime cap.
