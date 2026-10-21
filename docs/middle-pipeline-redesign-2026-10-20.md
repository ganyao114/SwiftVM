# Middle-pipeline redesign (2026-10-20)

Design for rewriting the SVM compilation middle layer: IR passes,
register allocation, and guest-register pinning. Based on
`docs/compile-stack-audit-2026-10-20.md` (stage map + measured
costs). Status: **proposal, awaiting approval**; no code yet.

## 0. Decisions already made

- Deliverable is this design first; implementation after approval.
- **In-place replacement**: the new middle layer replaces the old
  one at the same boundaries; no long-lived dual pipeline. The
  replacement still lands as a sequence of individually green steps
  (see section 9), each deleting the code it supersedes.
- **IR and HIR are retained**: opcode set, `ir::Inst` slab layout,
  HIR function/block structure, and the x86 frontend's lowering stay.
  Two scoped exceptions are approved and designed below: deletion of
  the pseudo-operation chain (section 3A) and incremental immediate
  forms where a builder fold needs them. Remaining suspected
  IR-level bottlenecks are listed in section 5 for separate
  discussion; nothing else in this design requires changing them.
- **Smart builder layer approved**: target-neutral folding moves to
  decode time (section 3, stage N0.5), following the FEX IREmitter
  model; target-specific immediate encodability stays in the
  backend action layer because SVM also targets riscv64.
- Emission (`JitTranslator`/`FunctionCodeObjectEmitter`) and publish
  stay; the allocation query surface they consume is preserved
  (section 6), so emission changes are mechanical adaptations, not a
  rewrite.

## 1. Problems being solved (from the audit)

Measured on CoreMark @120 iters (translate wall 2429ms): generic
regalloc 743ms, target planning ~550-620ms, IR opts 352ms, decode+IR
build ~400ms, emission only ~144ms. Structural causes:

1. Three independent whole-unit replay multipliers (loop-hoist A/B
   second allocation, bind-triggered `ReplayAllocation`, target
   contract retry with full re-Declare).
2. ~30 per-block plan builders, each walking the instruction list
   and building private fact containers.
3. Production verification stages that re-derive facts the planners
   already computed (width/zero-store certificates).
4. Id churn (`IdByRPO` x2-3 per function, `ReIdInstr` per block)
   invalidating id-keyed facts between stages.
5. Five IR passes as separate full walks; uniform elimination alone
   is half the stage.
6. Two compile shapes (standalone block vs function region)
   duplicated through every stage.

## 2. Design principles

P1. **One allocation run.** Nothing after allocation may request a
    replay. All constraints (pins, ties, fixed homes, hoist
    decisions) are inputs computed before the allocator runs.
P2. **Declare before allocate.** Pinning is expressed as constraints
    on intervals (precoloring, tied operands, fixed-use/def),
    never as post-allocation binding + replay.
P3. **One fact bundle, one walk.** Uses, liveness inputs, width
    facts, guest-state (fixed-home reads/writes), consumer facts are
    computed once per unit into a single `UnitFacts` structure and
    consumed read-only by everything downstream.
P4. **Stable ids.** Instruction ids are assigned exactly once, when
    the function region is finalized (end of decode). No stage may
    renumber; transforms that insert instructions use fresh ids from
    the tail (intervals keyed by id tolerate sparse tails).
P5. **Correctness by construction; verification is debug-only.**
    Facts that today are re-proven by verifier stages (width
    elision, zero stores, plan shape) are established where they are
    created and recorded as part of the fact bundle. A full-plan
    verifier remains available behind `SVM_VERIFY_PLAN` for debug
    builds and tests, off in production.
P6. **One compile shape.** A standalone block is represented as a
    single-block function region internally; block-only special
    cases exist only at the frontend boundary until the block is
    wrapped. (The existing single-block fast path in the allocator
    remains as an internal specialisation, not a second pipeline.)
P7. **Arena lifetime per unit.** All middle-layer scratch lives in a
    per-compile `MemArena` (existing infrastructure, `Reset()`
    reuse across units); no per-block heap containers in hot paths.

## 3. New pipeline

```
frontend decode (unchanged) + N0.5 smart builder (fold at append)
  -> N0 finalize: RPO + final id assignment (once)
  -> N1 fused local opts (residual cleanup only)
  -> N2 UnitFacts build (one walk)
  -> N3 allocation (linear scan + integrated pinning, single run)
  -> N4 action lowering (one walk over blocks)
  -> emission (unchanged, adapted queries)
  -> publish (unchanged)
```

### N0.5 Smart builder at decode time (FEX IREmitter model)

The x86 frontend's lowering stops calling raw append and goes
through a smart builder layer that maintains, per block, a dense
value-fact table indexed by append order (known constant value,
known width, zero/sign-extension provenance, current guest-register
SSA bindings). At each append it applies target-neutral rules only:

- constant arithmetic and identity folds (add/sub/logical/shift
  with known operands), immediate-form selection where the IR op
  has an `Imm` operand form,
- effective-address canonicalisation (constant base merge,
  displacement folding) within x86 semantics,
- suppression of canceling uniform load/store pairs and of width
  ops whose result facts are already known.

Target-specific encodability (which immediates fit ARM64/RISC-V
fields) is *not* decided here; that stays in N4, because SVM has
two backends. Facts recorded by the builder (constant/width tags)
are handed to N2 instead of being re-derived.

Effect on the rest of the design: N1 shrinks to residual cleanup
(cascades the builder cannot see locally, plus dead-code removal),
and the decode stage stops manufacturing the ~44% of IR nodes that
today are appended only for later passes to delete (149661 appends
vs 83478 published instructions in the audit run). Builder fact
invalidation is block-local and total: the table resets at every
label/terminal and on any call-like instruction, mirroring the
conservatism of the passes it replaces.

### N0 Finalize

`ComputeRPO` + single `IdByRPO`. Contract: ids are final for the
life of the unit. Frontend `ReIdInstr` calls during decode remain
(decode-local), but after N0 no renumbering is permitted; passes
that today rely on re-id (hoist apply/revert) are restructured per
section 4.

### N1 Fused local opts

Replaces UniformElimination/UniformStoreSink/FlagsElimination/
ConstFolding/IntegerWidthElimination/DCE as separate passes:

- One forward walk per block applying per-opcode rewrite rules
  (constant folding, width narrowing, flag-dead marking) with a
  small worklist for cascading simplifications within the block.
- Uniform handling becomes a pre-step only for blocks that actually
  reference uniforms (feature presence already tracked by
  `BlockAnalysisIndex`-style flags during decode finalize);
  uniform forward/DSE logic is kept but invoked per qualifying
  block inside the same walk, not as whole-function passes.
- DCE becomes liveness-driven deletion at the end of the walk
  using use counts maintained incrementally by rewrites (no
  separate def-chain collection pass).
- Order-sensitive interactions documented today (uniform first,
  DCE last) are encoded as rule priorities inside the walk; this is
  the one place where semantic equivalence needs care and gets a
  dedicated test pass (existing pass tests reused per rule).

Expected: 352ms -> ~120-150ms for N1 itself (one walk instead of
five, uniform gated to qualifying blocks); with N0.5 absorbing the
local folds at decode time, the combined decode+opt spend
(~750ms today) targets ~350-450ms.

### N2 UnitFacts build

One walk over all blocks producing, in dense id-indexed storage
(arena-allocated):

- def->consumers (from today's `BlockAnalysisIndex`, dense form),
- live interval endpoints (first/last use per definition),
- guest-state facts: fixed-home writes/reads per home with the
  existing `GuestStateMap` residence/version logic (kept, but built
  once here instead of per-planner `Analyze`),
- width facts per definition (entry widths + per-inst width chain
  facts; today recomputed in at least three places),
- memory/consumer classification per instruction (memory op kind,
  fault/observe, flag consumers, boolean selects),
- block feature flags (has memory/FPR/vector constants/...).

Everything downstream reads `UnitFacts`; no plan builder may walk
the instruction list to re-derive any of the above. `GuestStateMap`
as a separately-owned per-block analysis object disappears; its
algorithms move into this build.

### N3 Allocation with integrated pinning (single run)

Keeps the existing linear-scan core (interval building from N2
endpoints, active/inactive handling, spill/reload machinery,
coalescing passes). Changes what surrounds it:

- **Pins are precoloring.** Each guest-register home gets a
  residence timeline from N2 facts. A definition that writes home
  H becomes an interval precolored to H's host register for its
  resident span; a read of H becomes a fixed-register use when N2
  proves the resident value is still the home's content; elided
  reads/writes are recorded as constraints, not post-hoc bindings.
  This replaces Declare -> Bind -> Replay entirely: there is no
  `BindDeclaredGPRHomes`, no `ReplayAllocation`, no declaration
  restore.
- **Ties are interval constraints.** Today's tie families (narrow
  load, width chain, memory operand, induction, intwidth) become
  typed tie edges built in N2: hard ties (must share, correctness)
  and soft ties (coalesce if free). The allocator consumes them
  during assignment exactly like existing coalescing, in the same
  single run.
- **Hoist by cost model, not A/B.** `LoopInvariantHoistRecipe`
  analysis stays; the apply/reject decision uses the recipe's own
  spill estimate (extra live range pressure vs saved in-loop ops)
  computed from N2 endpoints. The IR is rewritten at most once,
  before allocation, and allocation runs exactly once. The recipe
  revert path disappears.
- **Contract rejection without retry.** The two current retry causes
  (FPR coalescing, constant-address cache) become allocator inputs:
  both are decided from N2 facts before assignment (disable flags
  computed up front), so assignment never needs to restart.
- Spill/reload selection, scalar-insert handling, spill eviction
  stay as they are inside the scan.

Expected: generic regalloc 743ms -> ~400-450ms. The win is not the
scan itself (30ms of scan core) but removing duplicate
collect/verify/coalesce restarts and the second hoist allocation;
call-count evidence today: 749 collect-live runs and 578 verifies
for 442 allocations.

### N4 Action lowering (replaces the plan fan-out)

One walk per block converting (allocation + UnitFacts) into the
per-block action set emission consumes: publications, copies,
narrow operations, memory addressing choices, flags handling,
backedge/region edge facts. Today's ~30 `Build*Plan` /
`Prepare*` functions collapse into per-opcode-family handlers inside
this single builder, sharing the walk and the fact bundle.
Handlers keep today's decision logic (narrow-bit rules, funnel
shifts, memory writeback selection, scalar FPR publication, etc.)
- this is a restructuring of where decisions are computed, not a
change to the decisions.

Outputs replace `GPRBlockPlan`'s many optional sub-plan pointers
with one `UnitActions` structure per block (flat vectors in the
unit arena, indexed by instruction id where emission needs random
access). Width/zero-store certifications are produced here as
byproducts of the handlers that already know the facts
(correctness by construction, principle P5); the standalone
verifiers move behind `SVM_VERIFY_PLAN`.

Expected: target planning ~550-620ms -> ~150-200ms (one walk,
no private fact containers, no verification re-walk).

### Emission contract (what does not change)

Emission keeps querying allocation results through a preserved
surface: per-value GPR/FPR location at a consumer, spill/reload
records, dirty masks per instruction, fixed-home definitions and
elision queries, per-block action lookup. The functions keep their
names and semantics on `RegAlloc` where feasible
(`GPRLocationAt`, `GPRUseHome`, `SpillReloadAt`, `DirtyGPR`, ...);
`GPRPlanViewFor` returns the new `UnitActions` view. Emission code
changes are mechanical (plan field renames), reviewed handler by
handler against the old plan fields.

## 3A. Pseudo-operation chain deletion (approved IR change)

Inventory (2026-10-20): six pseudo opcodes in two families.

- Flags family: `GetFlags`, `SaveFlags`, `BranchOnlyFlags`
  (`ir.inc:114-123`). Producers: both frontends (x86 lowering and
  `runtime/frontend/arm64`), `FlagsEliminationPass` (sole producer
  of `BranchOnlyFlags`). Consumers: flags elimination, narrow
  integer flags plan, flags token plan, pseudo flags plan,
  consumer plan, raw-carry analysis, `BlockAnalysisIndex` pseudo
  flags recording, ARM64 `translator_flags.cpp`/`translator_alu_scalar.cpp`,
  the interpreter, const folding.
- Multi-result projections: `GetResult`, `Div128Remainder`,
  `CpuidUpper` - second results of a two-result producer, today
  chained via `next_pseudo_inst` with the producer as operand 0.
  Consumers include the riscv64 pair-producer path
  (`pair_pseudo_producer.h`).

39 runtime files reference the pseudo machinery; the chain forces
special cases into every walk (`HasPseudoOperations`,
`VisitPseudoOperations`, `GetUses(exclude_pseudo)`, pseudo-aware
id/use accounting in `instr.cpp`).

### Deletion design (two phases, only phase 1 is required)

**Phase 1 - un-chain (required).** All six opcodes become ordinary
instructions in the main block list, in program order at exactly
the position their owner occupied (projections immediately after
their producer). They keep their opcode, operands and semantics;
the builder API (`Block::SaveFlags` etc., used by hundreds of test
sites) keeps its signature and simply appends a normal
instruction. Deleted outright: `next_pseudo_inst`,
`IsPseudoOperation`, `HasPseudoOperations`,
`VisitPseudoOperations`, `GetPseudoOperations`,
`GetPseudoOperationLink`, the `exclude_pseudo` parameter of
`GetUses`, and every walk that branches on them. Emission handlers
that today resolve a pseudo through its owner switch to the
operand-0 definition they already carry. `PseudoFlagsPlan` folds
into the N2 fact build (its inputs become ordinary instructions in
the walk). This phase is mechanical and semantics-preserving by
construction: order, ids (fresh tail ids per P4 during migration),
and use counts are unchanged in meaning.

**Phase 2 - explicit flags SSA (optional, separate approval).**
`SaveFlags` produces a flags SSA value; `TestFlags`/`GetFlags`/
flag-reading terminals consume it explicitly within a block.
Cross-block flags state stays implicit (existing flags-merge/token
machinery at boundaries), so no phi nodes are introduced. This
only pays off if, after phase 1, flags handling still dominates
handler complexity in N1/N4; it is not assumed by any other part
of this design.

Multi-result projections need no phase 2: as ordinary
instructions they are already explicit SSA consumers of their
producer.

## 4. Expected effect (estimates, to be gated by measurement)

| Stage | Today | Target |
|---|---|---|
| Decode+N0.5+IR opts (N1) | ~750ms | ~350-450ms |
| Regalloc (N3 core) | 743ms | ~400-450ms |
| Target planning (N2+N4) | ~550-620ms | ~250-300ms (incl. facts build) |
| Middle total | ~1680ms | ~770-900ms |
| Translate wall (@120) | 2429ms | ~1400-1600ms |
| Compile intercept | 5.229B ins | ~3.0-3.4B ins (-35-42%) |

Steady-state slope must not regress (gate: within noise of
1263.5 kins/iter); the redesign is not expected to change code
quality materially by itself. Tiered compilation (audit option A)
is explicitly out of scope here and can layer on top later: N1-N4
become the optimizing tier unchanged.

## 5. IR bottleneck candidates (for separate discussion)

Per the agreed boundary these are NOT changed by this design; they
are recorded because the middle layer keeps paying for them:

1. **`Value` cast-type side channel**: width facts live partly in
   `Value::cast_type`, partly in recomputed width analyses (three
   separate derivations today). A single width-fact owner on the
   definition would remove a class of re-derivation.
2. **HIRValue <-> ir::Value duality**: order-id translation layers
   (`GetOrderId`, use lists keyed twice) exist because ids were not
   stable historically (see `hir_builder.h:143-147`). With P4
   (stable ids) the duality may be collapsible, but that touches
   the frontend contract.
3. **Five-argument packed `Inst`** with `Operand` encoding two
   sub-values: several plan handlers spend their complexity
   unpacking operand shapes (`GetLeft/GetRight`, physical slot
   indices via `PublicIndex`). A wider or flatter encoding is an
   IR change with decode-side impact.
4. **Decode append cost** (outside the middle layer): ~193ms of
   IR append inside decode lowering. N0.5 reduces append *count*;
   the remaining per-append fixed cost would need a bulk-append
   builder API, which belongs to the frontend.

## 6. Replacement sequence (in-place, each step green)

1. Introduce `UnitFacts` build (additive); switch
   `BlockAnalysisIndex` and `GuestStateMap` consumers to it;
   delete the duplicated derivations it replaces.
2. Stable-id contract: remove post-opt `IdByRPO`; make hoist apply
   id-preserving (fresh tail ids). Gate: suite + slope.
3. Pseudo un-chaining (section 3A phase 1): six opcodes become
   ordinary list instructions; delete the chain API and every
   pseudo-aware walk branch across IR, both backends and the
   interpreter. Builder APIs keep their signatures so the test
   corpus is untouched. Gate: suite + slope (this step must land
   before steps 4 and 7 so their handlers are written once,
   pseudo-free).
4. Smart builder (N0.5) in the x86 frontend (arm64 frontend
   adopts the same builder API where it constructs IR): fact table
   + target-neutral folds; shrink the affected opts to residual
   cleanup in the same step. Gate: pass-equivalence sweep + x86
   fuzz + suite + slope.
5. Pin constraints as allocator inputs; delete Declare/Bind/Replay
   (`BindDeclaredGPRHomes`, `ReplayAllocation` call sites,
   declaration save/restore). Gate: `SVM_BINDCENSUS` parity on
   elision counts before removal, then suite + slope.
6. Hoist cost-model decision; delete second allocation and revert
   path.
7. Contract inputs up front (FPR coalescing / const-address
   disable flags from facts); delete the retry loop in
   `ARMTargetPlanner::Complete`.
8. Fuse remaining IR opts into the N1 walk; delete the five pass
   classes' block variants, keep function entry as the only entry.
9. Replace plan fan-out with the N4 action builder family by
   family (memory, narrow, flags, scalar FPR, region/backedge);
   move verifiers behind `SVM_VERIFY_PLAN`; delete `GPRBlockPlan`
   sub-plan pointers as each family lands.
10. Wrap standalone blocks as single-block functions at the
    frontend boundary; delete block-only pipeline variants.

Every step: full suite failure set identical to the RedMagic
baseline, CoreMark slope within noise of 1263.5, intercept A/B
recorded. Steps are ordered so each is independently revertible
even though no dual pipeline is retained.

## 7. Risks and open questions

- N1 rule-order equivalence (uniform/flags/const interactions)
  needs a dedicated equivalence test sweep; the existing pass
  tests are the starting point, plus CoreMark CRC + slope gates.
- Precoloring density: with up to ~16 guest GPR homes pinned, the
  allocator's free pool shrinks; the current post-hoc binding could
  silently fall back where precoloring must instead spill. The
  fallback rule (demote a pin to a soft tie when its residence
  conflicts) must be specified before step 3 - proposal: pins are
  hard only where N2 proves a single resident definition spans the
  use; all other home reads/writes are soft ties.
- Hoist cost model threshold needs calibration data (spill deltas
  from today's A/B runs can be harvested via existing counters
  before step 4 deletes the A/B).
- `SVM_SKIP_PREP` bisect switches name today's prepare steps; the
  redesign should keep an equivalent per-family kill switch set
  for regression hunting during the migration.
- Pseudo un-chaining touches every backend (ARM64 planners and
  translators, riscv64 pair-producer path, interpreter) plus both
  frontends' builders. It is mechanical, but it is the widest diff
  in the sequence; landing it as its own step (step 3) with no
  other semantic change keeps failures attributable. Serialized
  code (disk cache / AOT) encodes instruction streams, so the
  format version must bump with this step.

## 7. Implementation progress

### 2026-10-20 (evening): step 1 first slice landed (BAI sharing)

- `arm64::UnitFacts` introduced
  (`backend/arm64/alloc/unit_facts.h`), owned per allocation
  attempt by `RegAlloc` (`RegAlloc::Facts()`). Attempt scoping is
  what makes sharing sound: baseline and hoisted attempts own
  distinct RegAlloc objects, and an audit of every IR-mutation
  site (`SetArg`/`EraseInst`/`InsertBefore`/`AppendInst`/...)
  shows mutation only in `ir/opts` passes, never between
  `PrepareFixedGPRConstraints` and target planning.
- Consumers switched: both `PrepareFixedGPRConstraints` overloads
  (function overload no longer keeps a per-block index vector)
  and `GPRPlanBuilder::Prepare` (copy-assigns the shared index;
  its own `BuildWriteSuccessors` still runs on the copy, and
  `ReleaseUseFacts`/`ReleasePseudoFlags` semantics are
  unchanged, so the planner diff is zero). Shared build uses
  `collect_flags=true`, the superset both phases need.
  Write-successor data is explicitly NOT shared state: both
  `FollowingWrite` readers rebuild it with their own predicates
  immediately before reading (audited: exactly two readers).
- Gates (RedMagic bf1359ff): full suite 627 cases, 618 passed,
  9 failed / 67 assertions, failed-case name set IDENTICAL to
  baseline; CoreMark CRCs 0x4983/0xd340; slope 1264.0 kins/iter
  vs 1263.5 baseline (noise); compile intercept
  5.229B -> 5.201B (-0.53%, ~28M ins ~= 4.8k blocks x one
  avoided index build).

### Findings that resequence later steps

- **GuestStateMap sharing deferred.** Its block state is not a
  pure function of IR: `BuildValueVersions` takes
  allocation-dependent inputs (ignored publications, coalesced
  writes), and constraint preparation runs without
  `AnalyzeFunction` seeding while the planner runs with it.
  Sharing needs the pure-IR subset extracted first; not a
  drop-in cache like the block index.
- **Step 2 (stable ids) deferred until after N2/N3.** The
  linear-scan allocator uses instruction id order as its
  program-order coordinate for live intervals. Hoist `Apply`
  moves value definitions to the block head and
  `UniformStoreSink` relocates instructions; keeping stale ids
  would make id-space interval overlap under-approximate true
  simultaneity (unsound), which is exactly why `runtime.cpp`
  renumbers after Apply/Revert today. P4 becomes implementable
  once interval endpoints come from UnitFacts positions instead
  of ids. Direct cost of deferral is negligible: `id_rpo_pre`
  + `id_rpo_post` = 6.5ms of 2429ms translate (0.27%, device
  `svm_prof_full.log`).
- **Next: hoist A/B census, then step 6.** `collect_live`
  749 calls vs 431 units implies ~294 second allocations from
  the hoist A/B (multi-block units). No win/loss counters
  existed; a `[hoist-census]` line (anchors, gpr/const counts,
  baseline vs hoisted spill counts) now prints under
  `density_prof` at the A/B decision point in
  `PrepareFunctionRegion` to calibrate the cost model before
  the second allocation is deleted.

### 2026-10-20 (late): allocation multipliers censused; eviction futility cut

Step 6's original target (hoist A/B) turned out to be a
non-factor on CoreMark: 6 hoist recipes total (2 units), spill
counts 0 vs 0. The real "second allocation" multipliers, by
census (`[evict-census]`/`[replay-census]` under `density_prof`,
plus `evict_select`/`evict_collect` prof2 scopes):

- `collect_live` 751 calls for ~521 allocation invocations.
  495 invocations succeed in 1 attempt; **26 units burn 106
  eviction restarts (184 scans) and still fall back to the
  reserve ladder**. No unit ever verifies via the evict path
  after a restart. The eviction loop only re-runs
  `Verify()` when an attempt finds no candidate, so units
  whose real problem is an undersized scratch reserve pay up
  to 8 full scans before reaching the ladder that fixes it.
- Planner contract replay: 81 of 436 units replay once
  (step 7's target; unchanged by eviction settings).
- Eviction evaluation itself (`SelectEvictionCandidate` +
  `CollectSpillUses`): ~66ms of 2442ms translate. An exact
  fast path (cached kind/code per interval, monotone next-use
  cursors) measured no intercept change, so this remainder is
  mostly the extra use-list walk; it dies properly when N2
  supplies use lists from UnitFacts.

Landed: **eviction futility cut**. Attempts after the first
restart now run to completion (`finish_on_candidate`) and are
verified before another eviction is considered; if such an
attempt cannot verify at the current reserve, control goes
straight to the ladder instead of restarting. First-restart
quality behaviour (the synthetic corpus shapes) is preserved:
attempt 1 still aborts on first candidate, and a verifying
attempt with a candidate still restarts.

Gates (RedMagic): suite 627/618/9, failed-case set IDENTICAL;
CRCs 0x4983/0xd340; slope 1263.9 (unchanged); intercept
5.207B -> 4.792B (-7.98%), i.e. 82% of the way to the
`SVM_RA_SPILL_EVICT=0` floor (4.695B) without giving up the
eviction quality path. Cumulative intercept this session:
6.198B -> 4.792B (-22.7%).

Next: census planner rejection reasons (which contract bit,
predictable from facts?) for step 7; then step 3 pseudo
un-chaining.

### 2026-10-20 (late): replay attribution -- bind, not contract retry

`SVM_BINDCENSUS=1` on CoreMark: Bind calls=435,
**changed_calls=80** -- essentially all 81 planner replays are
bind replays from `BindDeclaredGPRHomes`; contract retry
(step 7's original target) fires ~0 times. Candidate classes:
transfer 82/82 changed, narrow 73/353 (reasons: no_old=67,
preserved=6, ok=280), writer 13/13, operand 0.

Sizing via the existing kill switch (`SVM_SKIP_PREP=targetbind`,
which also disables early-result-homes prep): intercept
4.792B -> 4.364B (-428M, -8.9%), slope unchanged (1263.7 vs
1263.9), CRCs correct. So the whole Declare/Bind/Replay pin
complex costs ~9% of compile while buying no measurable
steady-state density on this workload; its elision wins are
real but live in the synthetic corpus shapes. Step 5
(precoloring/soft ties as allocator inputs, census-parity
gate) is the next big slice -- larger than steps 6/7 turned
out to be. Step 7 (contract inputs up front) drops to
cleanup-only priority: the retry loop almost never fires,
so deleting it is simplification, not performance.

### 2026-10-20 (late): step 5a -- verify instead of replay after Bind

Exact replay cost (`target_replay` prof2 scope): 160.7ms /
81 calls. `VerifyFixedGPRConstraints` already validates
exactly what a replay would enforce (value placed in its
fixed home; no non-transparent writer of that home inside
the residence span, other values allocated to the home
counting as writers). Refactored it into a bool core
(`CheckFixedGPRConstraints`, asserting wrapper kept
bit-identical) and made `BindDeclaredGPRHomes` skip
`ReplayAllocation` when the current allocation already
satisfies every fixed definition it just recorded (checked
per pending block against that block's draft physical
writes; `RestoreTargetDeclarations` still runs either way).

Result: `replay_skipped=62` of 81 changed bind calls;
intercept 4.792B -> 4.640B (-3.16%); slope 1263.4 (noise);
suite failure set IDENTICAL; CRCs correct. Cumulative
intercept this session: 6.198B -> 4.640B (-25.1%).

Remaining bind-complex cost (~276M ins upper bound from the
targetbind kill switch): 19 genuine replays, the
BindingFacts walk itself, and early-result-homes prep.
Full step 5 (precoloring/soft ties, delete
Declare/Bind/Replay) remains the structural fix; the
demotion rule still needs finalising before it starts.

### 2026-10-20/21: step 3.1-3.4a -- pseudo un-chaining, batches 1-4

Adjacency was audited first and is NOT sound: a temporary
`SVM_PSEUDO_AUDIT` walk found owner->pseudo block distances
up to 33+ in the suite (CoreMark: 798/4025 owners at
distance 2 via an intervening `ClearFlags`), plus pseudos
physically before their owner. So the chain is replaced by
(a) an O(1) `pseudo_use_count` on `Inst` maintained in
`Use/UnUse` (`GetUses(exclude_pseudo)`, counter-based
`HasPseudoOperations`), and (b) per-phase use indexes:
`BlockAnalysisIndex` `HasPseudoUse/ForEachPseudoUse` for
planners (consumer/boolean-select/fixed-GPR/memory/pinned/
flags-token/funnel-shift plans, width-elimination pass),
`PseudoFlagsPlan::RecordPseudo` fed from the BAI walk, and
an allocator-side `FlagsDemand` table
(`ScanFlagsDemand(block, owner)`) threaded through
`ScratchBudget/ClassifyGPRContract/PreciseAddSubScratchBudget`
and spill/reload regions; `const_folding` computes
requested flags by block scan; `HasFlagsSavePseudo` deleted.
`ClearFlags` is not a pseudo (arg 0 is `Flags`, no owner);
the old plan's ClearFlags case was dead and `demand.clear`
now stays zero by design.

Gates per batch (RedMagic): suite 627/618/9 with the
failed-case name set IDENTICAL to baseline every time;
slope ~1263.5-1263.7 (noise); CRCs 0x4983/0xd340.
Intercepts: UC1 4.639B, UC2 4.637B, UC3 4.638B, UC4
4.649B -- un-chaining is structural (kills O(chain) walks
and the cyclic-chain failure mode), not an intercept lever
on this workload. Remaining at that point: emission-end
pairing (interpreter/riscv), the IR-signature link sample,
tests, then deleting `next_pseudo_inst` itself.

### 2026-10-21: step 3.5+3.6 (UC5) -- chain deleted

Emission-end pairing and the remaining chain consumers now
use `CollectPseudoOperations(block, owner[, opcode])`
(block.h): an O(block) scan for instructions whose arg 0
is the owner's value. Sites: interpreter `WritePairResult`,
riscv `EmitNativeCall` pair secondary, riscv duplicate-
marker verification (block threaded through the emission
contract wrappers), the frozen-plan IR signature (link
pointer replaced by the per-owner pseudo count, already
implied by the two use counts it samples), and all tests.
`next_pseudo_inst`, `GetPseudoOperations`,
`VisitPseudoOperations` and `GetPseudoOperationLink` are
deleted; `Use/UnUse` only maintain `pseudo_use_count`.
`Inst` shrinks by one pointer. No IR serialization exists
(the disk cache stores compiled code), so no format
version bump is needed. One test guarded the old contract
directly ("frozen IR admission detects ... pseudo sibling
relink": a same-owner `SetArg` used to reorder the chain
and change the signature); it now asserts the chain-free
contract -- same-owner refresh is signature-neutral, an
owner retarget is still detected.

Gates (RedMagic): suite 627/618/9, 67 assertions,
failed-case set IDENTICAL to UC4; CRCs 0x4983/0xd340;
slope 1263.8, intercept 4.660B (both noise vs UC4).
Step 3 is complete. Next decision: step 5 full
(precoloring/soft ties; demotion rule still to finalise)
or step 4 (smart builder folding at decode).

### 2026-10-21: step 5 (demotion rule approved) -- narrow/writer mostly pre-planned

Demotion rule approved by the user: hard pin only where a
single resident definition is proven to span the use; other
home reads/writes are soft ties. Bind failure attribution
(new `fail_*` fields in `SVM_BINDCENSUS`): all 19 residual
replays came from Bind-added narrow (73) and writer (13)
definitions; transfer and Prepare-planned definitions never
fail. A `SVM_SKIP_PREP=targetbind` suite run breaks 9 extra
codegen-contract tests, so Bind's definitions are
load-bearing for the synthetic corpus and blanket demotion
is not viable; the definitions must move earlier instead.

Root cause for narrow: `PrepareBlock`'s narrow definitions
were the only producer without `version=true`, making them
single-home candidates the allocator drops wholesale under
pressure (`FinalizeSingleHomePreallocation`); Bind then
re-planned the identical proof as a version and replayed
to honour it. Per-read clause tracking confirmed the early
matcher accepts every read Bind later adds (the difference
was never the matcher, the GuestStateMap seeding, or IR
drift). Fix (NV1): plan narrow reads with `version=true` --
the matcher plus `FixedHomeSurvives` is exactly the
approved hard-pin proof. NV2: `BindTargetDefinition` also
accepts a preserved definition whose prefix already covers
the requested span. A narrow home-affinity experiment was
reverted: alias reads cannot own intervals, and the
affinity path is gated on the default-off fixed class.

Census (CoreMark @120): narrow changed 73 -> 6,
`no_old` 67 -> 0, writer 13 -> 1, fail_calls 19 -> 6
(remaining: 6 preserved-span narrow + 1 writer).
Gates (RedMagic): suite 627/618/9 IDENTICAL; CRCs correct;
slope 1262.7 (noise); intercept 4.660B -> 4.463B (-4.2%).
Cumulative intercept this session: 6.198B -> 4.463B
(-28.0%). Remaining Bind complex (BindingFacts walk,
transfer planning, 6 replays) measures <= ~85M ins by
the targetbind kill switch; deleting it needs transfer
pre-planning, which has a documented code-growth trade
in `PrepareBlock`, so it stays for now.

### 2026-10-21: step 4 census -- where the builder must hook

Current phase split (`SVM_PROF2`, CoreMark @120, NV2):
translate 1.92s = decode 212ms (`ir_append` 48ms,
149704 appends for 83518 published instructions: 44% of
appended nodes die in opts) + passes 344ms (uniform 175ms,
const 83ms at 2 calls/unit, DCE 47ms, flags 41ms) +
regalloc ~560-600ms + codegen ~625ms (target planning
inside). Uniform traffic dominates appends: the pass sees
29109 loads / 18454 stores; guest GPR accesses funnel
through `X64Decoder::R(reg)` / `R(reg, value)`, and every
frontend append funnels through `ir::Assembler::AppendInst`
(ir_assembler.h) -- that is the smart-builder hook point,
no decoder-file churn needed.

Implementation plan for the first step-4 slice: a
byte-range value table inside `Assembler` mirroring
`UniformEliminationPass`'s forward contract (its barrier
set is the spec: `StoreUniform` range overlap,
`UniformBarrier`, call-like ops keyed by the Lambda
`UniformEffectId`, block boundaries via the Assembler
terminal forwards). `LoadUniform` appends consult the
table first; the pass stays as residual cleanup.
Equivalence oracle: published IR (`SVM_DUMP_IR_POST`)
must be byte-identical with the builder on/off over the
func corpus, plus the usual suite/slope gates. Width/view
extraction rules must match the pass exactly (partial
reads of a wider known value, AH-style inserts); start
with exact-width full-range hits only.

SB0 (shadow, landed): `ir::Assembler` now carries an
optional uniform byte-fact table (`EnableUniformBuilder`,
wired at both x86 decode sites, inert unless
`SVM_FOLDCENSUS` is set) that mirrors the pass's forward
walk at append time -- mapped-range handling, helper
effect invalidation, Goto/BindLabel path merges included.
It changes no IR; it only counts. CoreMark @120: builder
would_fold=15724 / would_map_loads=16743 /
would_map_stores=19672 vs the pass's own folded=13108 /
mapped_loads=14694 / mapped_stores=17004 (builder totals
run higher because re-decoded blocks are counted twice).
Suite IDENTICAL with the shadow present but disabled.
SB1 next: make the overloads actually return the
BitCast/BitExtract/GetHostGPR the pass would have
produced, gate on post-uniform IR dump equivalence over
the corpus, then suite + slope. The const side measured
too small to build first (scalar CSE dedups only 180
appends; immediate folds 3380 still append their LoadImm).

SB1 (landed, default on via `SVM_BUILDER_UNIFORM`): the
Assembler `LoadUniform` overload now emits exactly the
node the pass would have produced -- BitCast/BitExtract
fold alias or mapped GetHostGPR/FPR -- using the SB0
table. Stores stay StoreUniform for now (DSE/store-sink
ordering). Two equivalence subtleties were found by dump
diffing and fixed: folded loads are re-stamped with the
Uniform descriptor type at every append (decoder SetType
calls at arbitrary later points, e.g. MulWithFlags, must
not survive -- the pass's stamp is the final word), and
the flush runs after the appending instruction is created
so its own return-type derivation still sees decode-time
types. Post-uniform dump diff over CoreMark: 2667/3176
blocks byte-identical, 469 removal-only (extra dead
SetHostGPR the pass DSE can now prove), 11 conservative
divergences where a store the DSE pre-sweep deletes is
still visible at decode time and clears builder facts
(builder folds less, never differently).

SB1 initially regressed compile time badly (+570ms):
folded loads stop resurrecting uniform bytes in
`EliminateDeadStores`, so far more stores test all-dead
and each paid `HasObservableUse`'s whole-block rescan
(quadratic). Fixed independently of the builder: DSE now
builds a per-block consumer index lazily on first victim
test. Final gates (RedMagic, builder default on): suite
627/618/9 IDENTICAL (one config-registry count test
updated for the new field), CRCs correct, slope
1262.7 -> 1228.5 (-2.7% steady state, from the extra
dead-store wins), intercept 4.463B -> 4.327B (-3.0%;
DSE index alone accounts for ~43M of it). Cumulative
intercept: 6.198B -> 4.327B (-30.2%); slope
1658.8 -> 1228.5 since session start.

### 2026-10-21: steady-state density census (next battlefield)

Static taxonomy over all compiled blocks (`SVM_DENSITY_PROF`,
CoreMark @120, 4548 blocks / 82976 IR ops / 314944 host
bytes): work 39.4% of host bytes, boundary 34.3%, move/width
19.4% (but 49.6% of IR ops), flags 4.3%, uniform 1.6%.
Hot guest ranges (core_bench_list/find/sort 0x219700-0x21ab00,
bench_matrix, bench_state+crc) show the same shape:
boundary 32-37%, move 18-24%, work only ~35-40%, at
3.4-3.8 host bytes per IR op. Execution weighting still
needs block hit counts, but the two attack surfaces are
clear: (1) boundary bytes -- per-block prologue/terminal
sequences (split-flags entry branch, PC sync, link/exit
trampolines) charged once per block visit; (2) move/width
ops -- 41140 IR ops emitting 61064 bytes, i.e. ~15k real
mov/extend emissions survive all coalescing. Flags bytes
are already small (4.3%) after the lazy-token work.
Execution trace/counter emission is off by default and
not part of the slope.

Execution-weighted follow-up (same day): simpleperf
cpu-cycles samples from a @12000 run joined through
`[svm-host-map]` unit ranges (431 units) to guest
functions, then weighted by each block's static category
bytes. JIT-attributed cycles: core_state_transition
25.0%, core_bench_list 22.5%, matrix_test 8.5%, crcu16
3.3%. Weighted category shares of JIT cycles: boundary
40.7%, work 34.8%, move 14.3%, flags 9.4%, uniform 0.7%.
So the first density slice should target per-block
boundary sequences in small hot blocks (prologue +
terminal: split-flags entry, PC sync, direct-link
check/exit), not ALU codegen. Tooling used:
`SVM_VIXL_HOST_DUMP=1` (host bytes + unit map) and
`SVM_DENSITY_PROF=1` (per-block taxonomy); capstone
disassembly of the dumped bytes works for inspection.

First density probe (DN1, reverted): the hottest single
IP (14.3% of cycles) is the backedge exit poll
(`ldarb`+`cbz`, plus the exit-latch dummy load when the
latch is enabled). Two findings: (1) the poll has always
read **byte 0** of `exit_request` (the SMC generation),
not the top byte its comment claimed -- LDARB has no
offset encoding and VIXL silently drops the +7 in
release builds; signal/retirement bits reach the thread
via the armed latch fault and full-width trampoline
polls. The comment now says so. (2) `ldarb` -> plain
`ldrb` (byte 0, identical semantics) measured **no slope
change** (1228.5 vs 1227.7, noise), and reading byte 7
instead breaks the SMC-generation observation the RCpc
adaptive path depends on (slope exploded +35% in the
experiment). So the poll cost is the load+branch+dummy
load themselves, ~3 host instructions per backedge, not
the acquire. Real cuts need a poll redesign (e.g.
fault-handler-redirected RCpc exit so the generation
byte need not be polled, or a register-resident countdown),
not an encoding tweak. Six tests also pattern-match the
literal `ldarb` text to locate poll sequences; any future
encoding change must update those locators.

Move/width census (post-uniform IR, first occurrence per
block): ZeroExtend32To64 12591, BitCast 7561, BitExtract
7054, SignExtend 638 -- 11-19% of ops in hot ranges, but
only ~15k of 41140 move-class ops emit any host bytes
(61064 static bytes total); the rest are already
zero-cost aliases. The emitting residue is mostly real
W-zeroing the guest semantics require. Combined with the
boundary finding, the remaining steady-state gap to FEX
(slope 1228 vs 656) is spread across per-backedge poll
insns, per-block boundary sequences, and small residual
moves -- no single remaining slice is large; each needs
its own design (poll redesign is the biggest single item
at ~14% of cycles for one IP).

DN2 (landed): when the exit latch is enabled,
`PollCycleExit` now emits only the dummy latch load --
every exit_request publisher arms the latch page (the
RCpc adaptive generation bump was the one exception; it
arms now too), so the fault reaches the same service
veneer and the ldarb+cbz pair was dead weight on every
backedge. Result: slope 1228.1 -> 1186.7 (-3.4%
instructions) and, more tellingly, steady cycles/iter
463k -> 372k (-19.6%): the acquire load's ordering stall,
not its instruction count, was the real cost. Suite
IDENTICAL (terminal polls still emit ldarb, which is
what the encoding-locator tests key on). Remaining poll
surface: PollTerminalExit (unit boundaries, no dummy
load registered today) and the full-width trampoline
polls.

DN3 (landed): PollTerminalExit got the same latch-only
treatment. It now returns an optional FaultRange; the
translator registers it via RecordExitPollFault with the
poll's own target as a final guest_start (new
guest_final flag: ResolveExitPollFaults must not
overwrite it with the shared stub resume used for cycle
polls), and EmitTerminalRequestExitStub resolves those
faults with the same publish-frame recovery the cycle
stubs use. One test pinned the old placement rule
("latch dummy load only on descending edges"); its
encoding assertion now documents the new rule that all
exit polls are fault-only latch loads, while the
behavioral core (descending edge observes Signal) is
unchanged. Result: slope 1186.7 -> 1176.0, cycles/iter
372k -> 352.8k (-5.2%), intercept unchanged, suite
IDENTICAL. Remaining ldarb polls: the XRegister
(indirect-target) terminal overload and latch-disabled
units.

### 2026-10-21: planning-cost attack (keep the mapping)

A/B with SVM_UNIFORM_ELIM=0 (builder still on) priced
the uniform->GPR mapping precisely: intercept 4.33B ->
2.70B (-37.7%) but slope 1176 -> 1500.9 (+27.6%) and
cycles +31%; break-even ~5000 CoreMark iterations. The
pass's forwarding is fully subsumed by SB1 (fold census:
folded=0, mapped_loads=0), but its 14104 store mappings
create the pinned IR whose downstream planning costs
~636ms of the 1.92s translate (regalloc +284ms, codegen
middle +185ms, pass itself +167ms). Decision (user):
keep the mapping, attack the planning cost. Bind replay
was measured at 6.2ms total (7 of 436 calls) -- deleting
Bind/Replay is no longer worth its risk and is dropped
from the plan.

Phase probes (SVM_PROF2) named the codegen "middle":
CompleteARM64UnitForEmission is 436ms of 604ms codegen
(plan_verify 139ms, plan_declare 116ms, plan_perblock
110ms, plan_bind 28ms). Symbol sampling showed the rest
is diffuse: scudo malloc/free ~17% of compile cycles,
memset 2.6%, no single plan builder above 0.4%.

Landed slices, each gated suite 627/618/9 IDENTICAL +
CRCs + slope held at 1176:
- PK1 UniformStoreSinkPass::CaptureLatestCaptures:
  capture/clear only touched segment bytes instead of
  O(uniform_size) fill+scan at every fault point.
  pass_uniform 218 -> 145ms; intercept -4.5%.
- PK2 VerifyGPRAllocationPlan: owners hash -> dense
  id-indexed table, preservation-action empty guard,
  dominator scratch reuse. plan_verify 145 -> 113ms;
  intercept -1.6%.
- PK3 FixedGPRConstraintsHold: definitions hash ->
  sorted vector + binary search. verify_fixed 44 ->
  23ms, plan_bind 28 -> 20ms, regalloc_verify 56 ->
  31ms (same checker on three paths); intercept -3.7%.

Cumulative after PK3 (RedMagic): slope 1176.3
kins/iter, cycles ~351-357k/iter, intercept 3.919B,
translate 1.72s. Session arc: slope 1658.8 -> 1176.3
(-29%), intercept 6.198B -> 3.919B (-37%). FEX
(self-built, same device): slope 656.5, cycles 169.4k,
intercept 0.157B -- remaining gaps 1.79x / ~2.1x / 25x.
The intercept residue is now diffuse per-block planning
(~15 plan objects per block, shared_ptr + hash maps) and
heap churn; closing it needs the structural single-pass
planner of section 3 rather than more micro-slices.

Follow-up micro-slices (same day, all gated IDENTICAL,
slope held at 1176.1): PK4 Seal flags_demands hash ->
small vector and dead-write published_versions hash ->
small_vector (builder_dead 37 -> 31ms, seal 52 -> 50ms);
PK5 PrepareGPRPublicationPlan reuses PrepareMemoryUpdates
match results instead of re-matching (declare_tail ~-2ms);
PK6 REF-chain flattening at BeginTargetPlanning /
ReplayAllocation / FreezeGPRLocations (no measurable
change -- chains were already ~1 hop; kept as an O(1)
guarantee for ResolveId). Each moved the intercept by
<0.5% (final 3.912B, translate 1.75s), confirming the
diffuse floor: further compile-speed progress requires
the structural work, not more local fixes. Phase split
at the end (SVM_PROF2): decode 264ms, IR opts 307ms
(uniform+sink ~145ms of it), generic regalloc 461ms,
codegen 560ms (target completion ~330ms: declare 121 /
perblock plans 110 / verify 90 / bind 21), publish 46ms.

PK7 (landed, steady state): EA immediate folding for
scalar TSO accesses. Host-dump census showed 4249 of
7052 RCpc accesses preceded by a standalone EA add/sub
immediate. MemoryLoweringPlan now folds single-use,
pseudo-free EA Add/Sub with simm9 offset into the access
(new `offsets` action; EA instruction covered/skipped;
base must be pinned at the access or adjacent to it),
EmitMemOperand returns the offset form, and the TSO
load/store RCpcImm immediate gate widened from size==1
to !check_alignment (unchecked-mode faults are emulated
by TryEmulateRcpcFault, whose decoder already covers
LDAPUR/STLUR all widths; checked mode still re-
materializes via ComputeAddress, no loss). Result:
static host insns 76098 -> 73837 (-3.0%), 2398 RCpc ops
carry an immediate offset, slope 1176.1 -> 1156.3
(-1.7%), cycles ~352k -> 345.8k/iter, suite IDENTICAL.
Residual EA patterns: 1656 push/pop-shaped dual-use EAs
(the Sub result is also the published new RSP; folding
needs the Sub tied to the RSP home at its definition --
manual pre-index, since STLUR has no pre-index form --
saving the publication mov, ~1 insn per push) and 5
out-of-simm9 immediates. Evaluated and rejected: manual
pre-index breaks fault-precise RSP -- hardware writeback
suppresses the base update when the access faults, a
manual `sub` before the store cannot be rolled back, so
a faulting push would retry with RSP already decremented.
This is the same reason WritebackAllowed excludes RCpc.

Negative result (same day): replacing the XRegister
PollTerminalExit's Ldarb with a plain Ldrb (identical
byte read) measured no cycle change on indirect-dispatch
hot paths -- the cbz hot IPs there are plain load-to-branch
latency, not acquire ordering -- and broke one ldarb-
locator test, so it was reverted. DN2's win was removing
the sequence entirely at backedge frequency, not the
acquire itself. Session arc now: slope
1658.8 -> 1156.3, intercept 6.198B -> 3.909B.
