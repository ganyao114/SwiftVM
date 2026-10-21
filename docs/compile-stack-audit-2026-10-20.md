# Compile-stack audit (2026-10-20)

Full map of the SVM guest-to-host compilation pipeline with measured
per-stage costs, taken as the basis for a slimming redesign. Costs are
from a RedMagic (NX729J) CoreMark run at 120 iterations with
`SVM_PROF=2 SVM_PROF2=1 SVM_IR_DETAIL=1 SVM_DECODE_PROF=1`, single
core (`taskset 10`, ~1.165 GHz). Workload shape for that run:
431-435 compiled units, 4889 decoded blocks, 29423 guest instructions
decoded, 149661 IR instructions appended during decode, 83478 IR
instructions published, host_bytes=327780.

Reference gap (same device/guest, simpleperf): SVM compile intercept
5.229B host instructions vs FEX 0.157B total intercept (~0.079B
compile-only after subtracting the exit42 init floor). SVM spends
~1.08M host instructions per prepared guest block compiled (5.194B
compile-only intercept over 4806 prepared blocks = 137 single-block
units + 4669 blocks in function units; 4889 blocks were decoded).

All stage figures below were cross-checked counter-by-counter
against the source log (`svm_prof_full.log`, pulled from the device
on 2026-10-20); every cited millisecond value matches the recorded
nanosecond counters within rounding.

## Pipeline map

### Stage 0: dispatch and unit formation

- `Runtime::Run` (`source/runtime/backend/runtime.cpp`) exits JIT code
  on `HaltReason::CodeMiss`; the frontend is asked to decode from the
  missed guest PC.
- x86 frontend (`source/translator/x86/translator.cpp`,
  `function_region_decoder.cpp`, `function_decode_frontier.cpp`):
  region decode with an 8 KiB lazy decode limit
  (`DecodeLimit`), builds `ir::HIRBuilder` functions or standalone
  blocks, calls `Block::ReIdInstr()` at four sites, then hands IR to
  the backend through `TranslateIR` overloads.

### Stage 1: x86 decode + IR construction

- `x86::X64Decoder` (distorm-based length decode + hand lowering).
  Per guest instruction: fetch/validate, predispatch, lowering to IR
  via builder appends (`ir_append`: slab alloc + args + link + use
  registration), bookkeeping, PC advance, end commit.
- Cost: `decode_total` 385ms of 2429ms translate wall (16%);
  `decode_ns` coarse scope 450ms (18.5%). Inside: lowering 263ms,
  of which IR append 193ms; bookkeeping 43ms; fetch 11ms; raw length
  decode 14ms; distorm 6ms. Per-append subscopes over 149661 calls:
  alloc 26ms, value 24ms, use 23ms, args 22ms, link 18ms.

### Stage 2: RPO + id assignment (repeated)

- `PrepareFunctionRegion` (`runtime.cpp:1318`): guest-range prepare,
  `ComputeRPO`, `IdByRPO`, IR opts, then `IdByRPO` **again**, then
  regalloc; the loop-hoist path can add a third `IdByRPO` plus a
  second full regalloc (see findings). Cost itself is small
  (rpo+ids ~7ms) but it invalidates id-keyed facts repeatedly.

### Stage 3: IR optimization pipeline

- `PassPipeline::BuildDefault` (`source/runtime/ir/opts/pass_pipeline.cpp`),
  each pass in block and function variants, feature-gated:
  UniformElimination, UniformStoreSink (XmmFaultSink) + static FPR
  finalize, FlagsElimination, ConstFolding, IntegerWidthElimination,
  DeadCodeElimination (must stay last).
- Cost: `pass_total` 352ms (14.5%): uniform 177ms (forward 93ms,
  internal DSE 30ms), const 84ms, DCE 49ms, flags 40ms. Uniform
  elimination alone is half the stage.

### Stage 4: generic register allocation

- `RegisterAllocPass::Run*` -> `RunVerified` (from
  `register_alloc_verified.inc`) -> `LinearScanAllocator`
  (`source/runtime/ir/opts/linear_scan_allocator.h`, 2842 lines) +
  coalesce passes (`register_alloc_coalesce_{gpr,fpr,copy}.cpp`),
  const-address pass, spill/reload (`register_alloc_spill_reload.cpp`),
  CFG liveness (`register_alloc_cfg_liveness.cpp`).
- Cost: `regalloc_total` 743ms (30.6%): assign 163ms, collect-live
  151ms, verify 79ms, live scan 30ms, live values 16ms, sort 1ms;
  ~300ms unattributed inside the scope (coalescing and fixed-plan
  interaction). Call counts show repeated work: `collect_live` ran
  749 times for 442 `regalloc_total` calls, `regalloc_verify` 578.
- Loop-hoist A/B: when `LoopInvariantHoistRecipe` is non-empty,
  `PrepareFunctionRegion` runs the full regalloc a **second time**
  and keeps whichever spills less (`runtime.cpp:1378-1397`).

### Stage 5: ARM64 target planning (per block, ~30 sub-plans)

- Entry: `CompleteARM64UnitForEmission` ->
  `RegAlloc::CompleteGPRAllocation` ->
  `CompleteARM64TargetPlanning` -> `ARMTargetPlanner::Complete`
  (all in `source/runtime/backend/arm64/alloc/`).
- Declare phase per block (`DeclareGPRBlock`):
  `GuestStateMap::Analyze`, `GPRPlanBuilder::Prepare` (itself ~8
  prepare steps in `pinned_gpr_allocation.cpp` + `Prepare*` siblings
  in `pinned_gpr_*.cpp`), then `PreparePinnedLoadUpdates`,
  `PreparePinnedMemoryValues`, `PrepareSpilledGPRPublications`,
  `PrepareNarrowReads`, `PreparePinnedMemoryAddresses`,
  `PreparePinnedZeroExtendPublications`, `PrepareMemoryUpdates`,
  `PrepareGPRPublicationPlan`.
- Bind phase: `BindDeclaredGPRHomes` replays the entire allocation
  when any home binding changes (`allocation.ReplayAllocation()`),
  then restores target declarations.
- Target phase per block (retry loop, up to 2 replays + full
  re-Declare on contract rejection): FPR plan
  (`fpr_allocation_plan.cpp`), constant-address plan, flags-token
  plan, boolean-select plan, consumer plan, scalar FPR publication
  plan, memory lowering plan, integer branch plan, integer-flag
  preparation (`narrow_integer_flags_plan.cpp`), scalar preparation
  (resident/copies/raw-carry analyses), narrow-bit plan, funnel-shift
  plan, induction plan, zero-store verifier, then per-unit:
  `VerifyGPRAllocationPlan` + `GPRWidthCertificates` verification,
  region preparation plan, backedge flags plan.
- Cost: inside `codegen_total` 759ms scope together with emission;
  the fixed-capture scopes (`fixed_single` 27ms + `fixed_multi`
  390ms) cover the declare part. Emission itself is small (below),
  so target planning accounts for roughly 550-620ms (~25%).

### Stage 6: emission

- `FunctionCodeObjectEmitter` / `JitTranslator`
  (`source/runtime/backend/arm64/jit/translator_*.cpp`): prologue,
  block bodies, terminals, region edges, polls. Plan objects from
  stage 5 are consumed read-only here.
- Cost: body 89ms + prologue 21ms + terminal 34ms = ~144ms (~6%).
  Vixl emission is **not** a cost problem.

### Stage 7: publish

- `TranslatePreparedFunctionRegions` tail (`runtime.cpp:1454+`):
  code-cache allocation (with a full re-emit fallback when the unit
  exceeds the direct-link trampoline arena), flush, module push,
  per-block entry contracts (`FunctionEntryContract::Build` +
  canonical-terminal analysis), L2 publish, SMC registration,
  indirect-L1 fault entries, disk-cache record
  (`RecordJitCacheUnit`), link-target publish.
- Cost: `publish_total` 56ms (2.3%): flush 8.7ms, fault 9.5ms,
  L2 13.6ms, SMC 3.8ms.

### Cost ledger (translate wall 2429ms @120 iters)

| Stage | Wall | Share |
|---|---|---|
| Decode + IR build | ~385-450ms | ~17% |
| IR opt passes | 352ms | 14% |
| Generic regalloc (incl. coalesce/verify) | 743ms | 31% |
| ARM64 target planning (~30 plans/block) | ~550-620ms | ~25% |
| Emission (vixl) | ~144ms | 6% |
| Publish + RPO/ids + IR free | ~70ms | 3% |
| Unattributed/overlap | rest | ~4% |

Scopes nest (translate_single 666ms/137 units and translate_multi
1763ms/294 units are inclusive per-unit totals), so stage shares are
magnitude estimates, not a partition. In particular the fixed-GPR
capture scopes (417ms) straddle the regalloc and codegen scopes:
`CompleteGPRAllocation` first runs inside `RegisterAllocPass` and is
then re-entered (guarded by `target_complete`) inside the codegen
scope, which is why stage 5 is quoted as a 550-620ms range
(codegen_total 759ms minus the 144ms emission subscopes as the upper
bound) rather than a point value.

## Complexity findings

1. **Repeated whole-allocation replays.** Three independent replay
   mechanisms can redo large parts of a unit: loop-hoist A/B second
   regalloc, `BindDeclaredGPRHomes` -> `ReplayAllocation`, and the
   target-contract retry loop (replay + re-Declare every block, up to
   2 retries). Each is locally justified; together they make
   worst-case compile cost several times the single-pass cost.
2. **Per-block plan fan-out.** Stage 5 runs roughly thirty plan
   builders/analyses per block, most walking the full instruction
   list and allocating their own fact containers. Shared facts exist
   (`BlockAnalysisIndex`, `GuestStateMap`) but several builders still
   build private indexes (owned-index fallbacks) or re-derive use
   facts (`Collect*UseEnds` family in the allocator).
3. **Verification as a production stage.** `VerifyGPRAllocationPlan`,
   width certificates and the zero-store verifier re-walk the unit
   after planning; their outputs (certificates) feed back into the
   allocation, so they cannot simply be deleted, but they are
   structurally a second planning pass.
4. **Id churn.** `IdByRPO` runs 2-3x per function and `ReIdInstr`
   per block in the frontend; every re-id invalidates id-keyed
   facts and forces recomputation in later stages.
5. **Uniform elimination dominates the opt stage** (177ms of 352ms)
   and runs before flags/const/DCE each re-walk everything; the five
   passes are separate full walks with separate analyses.
6. **Decode append overhead is per-instruction fixed cost**: five
   subscopes (alloc/args/link/value/use) at ~20ms each over 150k
   appends; the IR-build fast path already exists
   (`SVM_IR_FAST`), the remainder is structural per-node work.
7. **Two compile shapes** (standalone block vs function region) are
   maintained in parallel through every stage (block/function pass
   variants, two planner entry points, two emission paths), roughly
   doubling surface area for each optimization.

## Slimming options (ranked, for approval)

A. **Tiered compilation.** Add a true baseline tier: decode ->
   minimal opts (or none) -> linear scan without target planners ->
   emit; promote hot units to the existing full pipeline. Largest
   possible intercept win (attacks stages 3+4+5 = ~70% for cold
   code), largest change: needs promotion triggers, deopt/invalidate
   wiring, and a code-quality floor for tier-0.
B. **Remove replay multipliers.** Decide loop hoist by spill-model
   heuristic instead of a second full allocation; make home binding
   incremental so `BindDeclaredGPRHomes` cannot trigger a whole-unit
   replay; fold contract-retry declarations into the first pass.
   Bounded, measurable via existing census counters
   (`SVM_BINDCENSUS`, collect_live call counts).
C. **Fuse stage-5 fact computation.** One shared per-block fact
   bundle (uses, width facts, guest-state, consumer facts) built in
   a single walk and consumed by all plan builders; delete private
   re-derivations. Medium change, keeps every plan's semantics.
D. **Fuse/gate IR opts.** Single-walk const+width+DCE; gate uniform
   elimination internals by block feature presence (partially done);
   profile uniform forward/DSE internals before rewriting.
E. **Verification merge.** Fold width/zero-store certification into
   the planners that already compute the underlying facts, keeping
   a debug-only full verifier behind a config flag.

Recommended order: B (bounded, immediate), then C, then A as the
structural step; D/E opportunistically. Each step is measurable
with the existing intercept A/B (simpleperf two-point slope) plus
`SVM_PROF2` stage scopes, and gated by the full test suite.
