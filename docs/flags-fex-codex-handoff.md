# Codex handoff: Align SVM flags with FEX

2026-09-14 mechanism work, based on `e6f3e9f`: see
[the implementation and remaining design](mechanism-evolution-2026-09-14.md).
This batch makes decoder ordering instance-owned, defaults the launcher to
software ordering, bounds allocator retries, and repairs ordering and spill
lifetime defects exposed by short guest checks. The linked document records
the exact validation scope; the broader compatibility work remains staged.
The next batch gives each x86 instance its own instruction reader and binds
string/x87 helper addressing to the active Runtime scope. It also corrects
the host-call scratch budget exposed by the two-instance regression. Global
mapping-check callbacks and transactional publication remain pending.

2026-09-13 correctness update: JIT buffer overruns and premature resident XMM
writes are fixed. The c-ray 1x1 case matches the current FEX reference in both
Linux memory modes with default optimizations. Eight focused test groups pass.
See [the current assessment](codegen-status-2026-09-12.md) for validation limits.

2026-09-12 measurement review: see [the current assessment](codegen-status-2026-09-12.md).
The historical range-weighted ratios below are structural estimates and must not
be cited as current dynamic instruction counts or speed ratios.

Date: 2026-08-29
Repo: `/Users/swift/CLionProjects/SwiftVM` (macOS). Linux direct runs on Orb: `ubuntu@orb`, tree `/home/swift/svm-phasec/SwiftVM`, build `/home/swift/svm-phasec/build`.
Author on git: `swift_gan`. **Do not push** until asked. English commits, no task IDs, no AI trailer.

**Do not inspect failed run `e333e444`** (xAI capacity / connection). Goal notices that name it are stale.

## Git / mission

- Product code tip: **`218383a`** `perf: combine cycle flags and reason trampolines`
- Tracked tree is clean before this documentation update. Preserve the existing untracked build/images/placement tools.
- Pi mission: `9306cb64-ce70-4726-a5e0-76fce2d23556` (goal mode ON). Rollback remains `SVM_FLAGS_REGS=0` (`ParseNonZero`; unset → ON).
- `npm:pi-codex-goal` is installed user-wide; `/goal` tools need a **new** Pi session.

## What already landed (read these first)

Default **`SVM_FLAGS_REGS=1`** (`121620f`). Region edges default ON. Default region window is **64** blocks (`SVM_FUNC_LAZY=1` means that window; `2..127` override; `<=0` eager 1024).

| Commit | What |
|---|---|
| `9ac80fd` | Share the compact byte movemask hierarchy with inline SSE4.2 string-result collection |
| `8c4f055` | Replace the 11-instruction byte `VecMovMask` shuffle tree with an 8-instruction `USHR`/`USRA`/`XTN` hierarchy |
| `26c5f40` | Share the halt-reason/return tail across direct-cycle cold exits in functions with at least five candidate stubs |
| `4c9c669` | Lower 8/16/32-bit DIV/IDIV to native 64-bit division and arithmetic remainder; retain helpers only for 128/64 division |
| `30c85c7` | Replace BSF/BSR preserve-all helpers with U64 count-zero IR lowered to native AArch64 `CLZ` and `RBIT + CLZ` |
| `4661866` | Feed an exact low U8/U16 store extract from an allocator-coalesced fixed-home publication directly to `STRB/STRH` |
| `a548ece` | Omit an adjacent narrow self-extension when the shared register is already proven zero above the width by a load/zero-extension chain |
| `53333fc` | Fold an exact adjacent U8/U16 low extract and `ZeroExtend32` into one `UXTB/UXTH` even when their allocated registers differ |
| `9d299aa` | Emit a sole adjacent U8/U16 memory-load extension directly into the consumer register even when linear scan assigned distinct registers |
| `c87a2a1` | Keep an exact narrow load's ordinary saved-flags zero-test alias on its pinned home instead of copying it to a temporary W register |
| `7194669` | Publish a signed U8/U16 memory load directly into its fixed GPR home and keep audited low-32 SignExtend/Mul consumers on that home |
| `d5e45c7` | Keep a post-publication full-width load alias on its pinned home when its sole later role is a memory address |
| `38cea6e` | Feed the known zero-extended W result of LDRB/LDRH directly to a dead narrow immediate branch compare |
| `b734438` | Follow one static direct call in the dead-successor flags proof when the callee overwrites every incoming flag before control flow, and track the inspected callee prefix for SMC |
| `ea82571` | Reuse one displacement-adjusted indexed effective address across the load and store halves of a basic integer memory RMW |
| `f8cc6ed` | Keep an exact narrow load's branch-only zero-test alias on the pinned publication home and remove the intervening W move |
| `e959074` | Compose low-extract input forwarding with dead narrow immediate branches so the specialized compare reads the original source |
| `162a4d8` | Emit a spilled U32 Add directly into its pinned publication home and keep proven post-publication low-32 ALU uses on that home |
| `b5936ac` | Admit a single adjacent carry inversion on a dead U8/U16 memory-operand `Sub` branch when the condition does not read carry, then discard the irrelevant normalization |
| `9fb39ef` | Retain an arithmetic result token only when parity is live; NZCV-only narrow producers no longer restore result bits solely for a dead PF token |
| `f42b7cc` | Fold `BitExtract(SignExtend(v32),0,32)` back to the original 32-bit SSA and remove both signed low-32 round trips through DCE |
| `a627d94` | Feed an adjacent single-use low U8/U16 extract directly to narrow flags alignment, where the existing left shift already discards high bits |
| `3f079f9` | Generalize the dead narrow immediate branch recipe from exact ZF to the existing ZF/CF/ZF+CF dead-edge conditions using `UXTB/UXTH; CMP imm` |
| `5922091` | Lower a dead-edge U8/U16 `Sub` with an immediate and exact ZF-only branch into `SUB imm; TST width-mask`, suppressing the single-use immediate materialization |
| `e2bb2c7` | Omit the final narrow-result `LSR` after branch-only Add/Sub/Neg when the arithmetic value has no ordinary use; observed results keep the truncation |
| `4cc2c1e` | Publish an exact U8/U16/U32 memory load directly into its pinned GPR home and omit the redundant zero-extension and host-register publication instructions |
| `14e48c1` | Extend the fixed-home copy builder through exact U8/U16 `ZeroExtend32` chains and emit one `UXTB`/`UXTH` from the source home to the target home |
| `942a63d` | Generalize the pinned low-32 self-write proof into a cross-pin copy: an exact U32 fixed-home read, zero extension and full fixed-home publication become one `mov wTarget, wSource`, while later aliases remain eligible only as proven memory addresses |
| `121620f` | FLAGS_REGS default ON |
| `a278d8d` / `5b7857d` / `6009f8f` / `f2b490d` | Region If skip: both successors cover incoming NZCV; transparent `mov/ret`; `ClearFlags` covers C/V; **`BranchOnlyFlags` covers full PSTATE NZCV** (IR mask is Jcc live subset, not the ALU write) |
| `73f6719` | Copy PSTATE at dispatcher **only if `region_edges_active`**; HostExit always |
| `85e22ae` / `82fa7c7` | Region window 16→32→**64** |
| `15e5165` | `lazy = budget <= kMaxFuncBlocks` (128). **`SVM_FUNC_LAZY=128` with `<` was eager and re-decoded L2 → 31B host** |
| `0535615` / `7397960` | JA → `b.hi`, JBE → `b.ls` |
| `601185d` / `52b0b6a` | L2 Unpark from **x26**; counted-entry veneer |
| `3641e32` | Full-width `LoadMemory` publishes directly into a pinned guest GPR home when the existing local last-use, observer, conflict, and width proofs all hold |
| `df5a54e` | Fail closed before reading non-And/Or carry-test args; reject packed-flags consumers from region transparent/cover proofs |
| `a91697b` | Hand a pinned W-view bridge directly to a proven destructive U32 result published back to the same fixed home |
| `d0578b6` | Fold adjacent single-use `BitExtract(0,32) -> ZeroExtend32To64` into one non-destructive W move; fix temporary-container iterator UB in width proofs |
| `11abed9` | Replace local `BitExtract(ZeroExtend32To64(v32),0,32)` round trips with the original U32 SSA and let DCE remove both redundant nodes |
| `ff42917` | Fold `BitExtract(v32,0,32)` into the original 32-bit SSA only for proven W-width consumers; reject narrow and opaque ABI uses |
| `7110d20` / `7045d3b` / `431be30` | Coalesce complete legacy scalar FPR results, compact full-NZCV publication, and publish VecZip results in place |
| `fa1768a` | Reuse the retained dynamic return target in both RSB pop formats instead of reloading `State::current_loc` |
| `729b826` | Route same-module static `SetLocation + ReturnToDispatch` exits through tracked direct-link sites, with the prior L2/dispatcher fallbacks retained |
| `24f9d49` | Skip RSB pushes in indirect-L1 modules and route retained return targets through the signal-safe inline L1; L1-off modules keep the exact RSB path |
| `2829a8d` | Back each active RSB with a 4 MiB guarded mapping and recover lower/upper guard faults by resetting the live x25 pointer in the interrupted context |
| `b3998d5` | Use the next region block for cycle-polled conditional layout without falling through into per-block cold stubs |
| `3ec9582` | Pair the production indirect-L1 request and cache-base loads; confirm observed signals through the shared acquire-checking trampoline |
| `e74e734` | Keep a complete V128 producer in its resident home through safe post-publication SSA uses; reject later home writes and multi-home remaps |
| `030f52d` | Replace an adjacent low-load/high-zero resident publication with one fault-exact D-register load after a full observer and alias proof |
| `97009a3` | Publish legacy scalar sqrt results through a dead resident merge home; independently reprove the fixed read, last use, and publication window |
| `f99eabf` | Reuse a dead fixed left home for legacy scalar FP binaries while preserving the high lane through a reserved temporary |
| `b692fca` | Make direct absolute `GetOperand` materialization the default; retain `=0` as the code-shape rollback |
| `c15a712` | Combine compact FCMP parity publication and AF clearing into one proof-backed bitfield insert |
| `2f2fb88` | Defer trailing constant `SetLocation` publication to dispatcher misses while preserving mid-block and dynamic observers |
| `523d679` | Write consumer-proven compact FCMP ordering directly into the PF/AF carrier and remove the separate publish insert |
| `e1257d6` | Stop emitting host instructions for semantic IR Nops while preserving decode/translate metadata effects |
| `b1e501e` | Let single-use integer zero values publish directly into fixed FPR lanes from `wzr/xzr` |
| `215a059` | Elide a shared integer zero when every use is a compatible uniform, memory or fixed-FPR store |
| `21000f1` | Extract EQ/NE, CS/CC, MI/PL and VS/VC directly from the saved flags register without restoring host NZCV |
| `0fc245c` | Encode direct-mode `[base + imm]` accesses with AArch64 scaled load/store offsets when possible |
| `723ace5` | Materialize single-bit N/Z/C/V `TestFlags` values with direct bit extraction while preserving live PSTATE |
| `8ca4b0b` | Materialize zero/nonzero without clobbering pending PSTATE on straight-line IR paths |
| `aa7b83d` | Read a live PSTATE single-bit flag directly with non-clobbering `CSET` |
| `a505485` | Lower encodable negative GetOperand displacements directly with `SUB` |
| `d9eb980` | Align direct-hash L1 storage and form each 16-byte entry address with one `BFI` |
| `e2f9527` | Preserve direct-mode `[base + index]` in memory IR and use the AArch64 register-offset encoding directly |
| `4821182` | Fold fault-exact direct-mode stack pushes into one AArch64 pre-index store; retain biased-memory and base/data-overlap paths |
| `1a69a59` | Fold an direct-mode fixed-base load plus the following dead-flags +1 base update into one fault-exact pre-index load |
| `6b10c73` | Share one computed 4 KiB guest page base across encodable absolute memory addresses and make the proven path default |
| `e10fec4` | Let one audited consumer reuse a pinned W view for every operand occurrence and feed callee-saved pinned values directly into sign extension |
| `7620306` | Store a sole narrow pinned GPR read directly from its fixed W home while preserving capture and address-use semantics |
| `e2fe71c` | Adjust carry to a Direct cross-block ABI on FlagM hosts and remove the polarity-byte publication path |
| `a9f5ddf` | Delete `InvertCarry` and its covered carry publication when backward liveness proves a later in-block C write wins before every read |
| `2d86a6e` | Screen candidates with bounded short shape runs and retained formal weights before promoting them to long benchmarks |
| `5a47163` | Collapse narrow logical flag identities into one width-correct NZ producer |
| `477947d` / `e389f9d` | Merge contiguous partial NZCV with bitfield instructions and share the optimal merge across region materialization |
| `5b94050` | Restore NZCV directly from the packed flags register; `MSR NZCV` ignores every non-NZCV bit |
| `183bf78` | Skip published-region NZCV restores when the existing target proof covers all incoming flags before any observer or fault |
| `46197f6` | Clear the contiguous AF/unused/C/V span with one bitfield clear |
| `4b0eb1d` | Keep compact COMIS relations in host NZCV across audited MOVSD instructions and consume them directly at the following Jcc |
| `d934979` | Extend the same relation lifetime across the vector move family lowered entirely by audited NZCV-preserving operations |

Hot files:

- `translator_region.cpp` — `SuccessorCoversIncomingNzcv`, `BlockIsFlagsTransparent`, `EmitRegionIf`
- `translator_flags.cpp` — `MergeNZCV`, `force_ret_pstate`, direct simple `CondSet` extraction
- `translator_flags_abi.cpp` — published region entry restore and target-kill reuse
- `trampolines.cpp` — runtime flags park/unpark ABI
- `translator_terminal.cpp` — generic If / LinkBlock / RSB
- `translator/x86/translator.cpp` — `RegionFuncBudget`, `kMaxFuncBlocks=128`, lazy skip of published L2
- `register_alloc_coalesce_gpr.cpp` — pinned guest GPR read/write coalescing and full-width load publication
- `register_alloc_coalesce_copy.cpp` — exact adjacent low32 copy-chain ownership
- `integer_width_elimination_pass.cpp` — local zero-extend/low32-extract round-trip elimination
- `register_alloc_coalesce_fpr.cpp` — resident-home interval proof and publication ownership
- `translator_fpr_publication.cpp` — scalar load/zero-high pairing and observer proof
- `translator_mem.cpp` — independent host-FPR publication proof and final bridge emission
- `translator_operand.cpp` — shared zero-store register eligibility and scaled/unscaled memory operand formation
- `translator_alu_vec_fp.cpp` — scalar-unary merge emission and redundant self-copy suppression
- `svm_config.h` — `flags_regs` default true; `region_edges` bounded64
- `tools/svm-linux-cq/quick_shape.py` / `weighted_diff.py` — bounded short shape capture and formal-weight comparison

## Fast benchmark screening

Do not run formal smallpt/c-ray/CoreMark/STREAM for every candidate. Use
`docs/codegen-benchmark-fast-path.md` and commit `2d86a6e` first: capture baseline and candidate
shapes with the same short input, apply retained formal entries through the strict PC/version join,
require at least 99.9% host-weight coverage plus all top-20 PCs, and compare the short oracle
byte-for-byte.

The calibrated smallpt screen is `smallpt_wh_x64 4 8 6`: 7.2–8.4 seconds on Orb, 99.983053%
formal host-weight coverage and top-20 20/20. Arguments below 4 are invalid because this guest
divides spp by four and executes zero samples. The screen runs without `SVM_DENSITY_PROF`,
`SVM_PROF=2` or `SVM_EXEC_PROF`; only the existing hot-shape collector is enabled.

## Honest density (coremark `0x0 0x0 0x66 20000 7 1 2000`)

This is a promoted-stage/formal gate, not an iteration gate. Measure on Orb **without**
`SVM_EXEC_PROF` (it inflates host):

```
timeout 45 env -u SVM_JIT_CACHE $SVM $FT          # checksum 9f52b7d59285dbe5, rc=101
timeout 90 env -u SVM_JIT_CACHE -u SVM_EXEC_PROF \
  SVM_DENSITY_PROF=1 SVM_RA_HOT_COALESCE_ALL=1 SVM_RA_HOT_COALESCE=/tmp/x.hot \
  $SVM $BIN/coremark_x64 0x0 0x0 0x66 20000 7 1 2000
# CRC 0x382f
```

`$SVM=/home/swift/svm-phasec/build/source/translator/linux/svm_translator_linux`  
`$FT=.../func_tests_x86_64`  
`$BIN=/mnt/mac/Users/swift/CLionProjects/SwiftVM-bench/bin`

Synchronize all tracked sources Mac → Orb before cmake. A touched-files-only copy left Orb's
`translator_terminal.cpp` stale during this continuation and produced a false c-ray region diagnosis.

| Config | host_dynamic | notes |
|---|---:|---|
| FLAGS=0, window 16 (old baseline) | **7.383B** | compare-to |
| FLAGS=1, window 64 (before `3641e32`) | **6.348B** | **−14%** vs old FLAGS=0/16 |
| FLAGS=1, window 64 (after `3641e32`) | **6.300B** | `6,347,614,988 → 6,299,957,711` (**−0.751%**) from direct load publication |
| FLAGS=1, window 64 (after `a91697b`) | **6.251B** | `6,299,957,565 → 6,250,517,460` (**−0.785%**) from pinned W-view handoff |
| FLAGS=1, window 64 (after `d0578b6`) | **6.210B** | `6,250,517,196 → 6,210,114,929` (**−0.646%**) from adjacent low32 copies |
| FLAGS=1, window 64 (after `11abed9`) | **6.087B** | `6,210,114,929 → 6,087,169,543` (**−1.980%**) from local width round trips |
| FLAGS=1, window 64 (**current default**) | **6.034B** | `6,087,169,543 → 6,034,267,121` (**−0.869%**) from safe same-width extracts |
| FLAGS=0, window 64 (**current rollback**) | **6.712B** | CRC `0x382f`; FLAGS remains **−6.1%** at the same window |
| FLAGS=1, RE=0 | 26.009B | vs FLAGS=0 RE=0 **26.774B (−2.9%)** |
| FLAGS=1, window 32 | 6.882B | |
| FLAGS=1, window 128 (after lazy fix) | 6.348B | same as 64; coremark hot funcs fit 64 |

After If-skip, `SVM_FLAGS_REGS_AUDIT=1` on window-32: **PStateClobber/RegionInternal ≈ 2.3k entries**. Remaining ~70M “flags audit” is **L2 `ldr` cache-reload** (Dispatcher/RSBHit), not MergeNZCV.

Move bucket is now **32.136%** of host (`move_dynamic = 1,939,191,376`). `11abed9` removes
122.945M common-PC host instructions from CoreMark after `d0578b6`; `ff42917` removes another
52.903M. Across both stages spill remains zero. The same-width stage removes 512 / 2,540,061 /
402,107 common-PC host instructions from STREAM/smallpt/c-ray. Remaining move volume is not
automatically removable W-alpha space.

Current RE=0 same-PC refresh, reusing the unchanged FEX `f2e35f3` blockstats and old guest/entry
denominators at >99.99997% coverage: CoreMark **3.613/1.807 = 2.000×**, STREAM
**2.161/3.336 = 0.648×**, smallpt **3.377/1.549 = 2.180×**. Current c-ray covered only 74.47%
of the old entry table, so no whole-workload ratio is claimed for it.

Validation for `3641e32`:

- Mac and Orb targeted RA/fault tests pass: 353 and 21 assertions.
- FLAGS `0/1` × function/block/interpreter func_tests all return 101 with checksum `9f52b7d59285dbe5` and identical output SHA-256.
- Function fingerprint A/B against the exact pre-change binary passes for 1661 units over 11 guests. The checked-in Linux golden predates the region-window changes and is already stale; do not update it as part of this RA change.
- Full `swift_test` has the same existing 48 assertion failures before and after this change, in the same file sequence; the change adds only passing assertions.

Validation for `df5a54e` / `a91697b`:

- New pinned W-view test: 6 assertions; GPR coalescing 353, width-chain 23 and resident-fault 21 all pass.
- FLAGS `0/1` × function/block/interpreter func_tests: rc=101 and checksum `9f52b7d59285dbe5` in all six cells.
- helper-fault 38/0; clone futex/lock under FLAGS `0/1` all rc=0.
- Function fingerprint A/B against the exact pre-handoff binary: 1664 units over 11 guests, PASS.
- Orb full suite with fixed RNG has the same 36 existing failed cases before/after; focused new and RA/fault tests pass.
- Full metrics and the current FEX gap assessment are in `docs/codegen-gap-refresh-2026-08-23.md`.

Validation for `d0578b6`:

- New low32 copy test: 6 assertions; GPR coalescing 353, width-chain 23, resident-fault 21 and
  W/X high-half 17 all pass.
- FLAGS `0/1` × function/block/interpreter func_tests: rc=101 and checksum
  `9f52b7d59285dbe5` in all six cells.
- helper-fault 38/0; clone futex/lock under FLAGS `0/1` all rc=0.
- Function fingerprint against exact `f8426db`: 1664 units over 11 guests, PASS; smallpt output
  SHA-256 is identical in both arms.
- Fixed `SWIFT_FUZZ_SEED=123456`: baseline 40 failed cases / 53 assertions, candidate 39 / 52.
  Two width-chain assertions turn green; remaining differential failures are the same VIXL tail
  disassembly self-consistency class, not a semantic regression.

Validation for `11abed9`:

- New width pass: 2 cases / 8 assertions. Combined low32/int-width/width-chain/GPR/fault/high-half
  focus: 8 cases / 474 assertions, PASS on Mac and Orb.
- CoreMark: `6,210,114,929 → 6,087,169,784`; common-PC host `-122,945,220`,
  move `-122,945,213`, spill 0→0, CRC `0x382f`.
- STREAM/smallpt/c-ray common-PC host: `-5,854 / -868,884 / -1,283,611`; no workload grows
  in total. smallpt PPM SHA-256 and c-ray canonical PNG IDAT MD5 match across arms.
- FLAGS `0/1` × function/block/interpreter func_tests: rc=101 and checksum
  `9f52b7d59285dbe5` in all six cells; helper-fault 38/0; clone futex/lock four cells rc=0.
- Fingerprint self-consistency: 1664 units / 11 guests. Against the exact old binary, the unique
  guest-PC set is unchanged; 400 units reduce IR, 0 increase, total IR `-2,025`.

Validation for `ff42917`:

- CoreMark: common-PC host/move `-52,902,689`, spill 0→0, CRC `0x382f`; current raw host is
  `6,034,267,121` and move is `1,939,191,376`.
- STREAM/smallpt/c-ray common-PC host: `-512 / -2,540,061 / -402,107`; smallpt PPM SHA-256
  and c-ray IDAT MD5 match their exact baselines.
- Width/fault focus including the U16 CallLambda regression: 9 cases / 482 assertions, PASS.
  FLAGS six-grid, helper-fault 38/0 and clone four-grid remain green.
- Fingerprint self-consistency: 1664 units / 11 guests; every per-guest unit and decoded-block
  total is unchanged, aggregate IR is `-1,046`.
- Fixed seed full suite returns to the pre-stage 179 passed / 35 existing failed cases and
  1,047,523 passed / 45 failed assertions. The unsafe generic prototype had added one U16 helper
  failure; the final consumer whitelist removes it.

Validation for `7110d20` / `7045d3b` / `431be30` / `fa1768a` / `729b826` / `24f9d49` / `b3998d5` / `3ec9582` / `e74e734` / `030f52d` / `97009a3` / `f99eabf` / `b692fca` / `c15a712` / `2f2fb88` / `523d679` / `e1257d6` / `b1e501e` / `215a059` / `21000f1` / `0fc245c` / `723ace5` / `8ca4b0b` / `aa7b83d` / `a505485` / `d9eb980` / `e2f9527` / `4821182` / `6b10c73`:

- Current formal smallpt is `smallpt_wh_x64 8 128 96`; do not substitute the fixed 1024×768
  `smallpt_x64` when updating the formal FEX ratio.
- Formal smallpt default-region host:
  `1,321,651,162 → 1,303,939,990 → 1,297,980,655 → 1,296,969,640 → 1,246,900,800 → 1,201,575,549 → 1,201,372,215 → 1,199,466,420 → 1,187,471,711 → 1,168,614,398 → 1,165,502,656 → 1,163,020,553 → 1,141,267,073 → 1,126,372,521 → 1,096,331,217 → 1,081,436,665 → 1,072,445,284 → 1,068,863,253 → 1,064,572,872 → 1,060,138,659 → 1,060,040,252 → 1,052,965,418 → 1,047,125,252 → 1,043,588,497 → 1,042,807,357 → 1,040,901,562 → 1,040,846,721 → 1,001,905,579 → 984,381,707`;
  cumulative `-337,269,455` (`-25.5188%`), spill 0 throughout. The arrows are full-NZCV
  compaction, VecZip resident publication, retained RSB target reuse, static-exit direct-link,
  default return-L1, cycle-polled successor layout, paired indirect-L1 state loading, then live
  resident-FPR publication, scalar-load FPR fusion, scalar-sqrt resident publication, then
  legacy scalar-binary resident publication, direct absolute-address materialization, compact
  FCMP non-NZCV publication, trailing static-location cold publication, compact FCMP carrier
  publication, semantic Nop elision, zero-register FPR lane publication, shared zero-store
  materialization elision, direct simple-condition extraction from saved flags, then scaled
  immediate load/store addressing, direct single-bit flag tests, PSTATE-preserving zero tests,
  one-instruction live-PSTATE flag materialization, direct negative-displacement lowering, then
  aligned L1 entry formation with `BFI`, register-offset memory EA preservation, then fault-exact
  stack-push pre-index stores, then same-page constant-address base reuse.
  `7110d20` is neutral here but saves
  452,646,984 on fixed 1024×768
  smallpt.
- RSB reuse shrinks 328 formal smallpt PCs with no growth. EXEC_PROF records 8,080,989 hits / 248
  misses, so the earlier 0.076% static heuristic was not an execution ceiling. c-ray equal-entry
  delta is `-414,747` with 967 PCs smaller and none larger; CoreMark equal-entry delta is
  `-25,442,605`. A call-dense workload is exactly `-64,000,000` under both RSB frame formats.
- Earlier c-ray equal-entry common-PC deltas: full-NZCV `-999,520`, VecZip `-124,308`; no common PC
  grows. CoreMark after full-NZCV is about `5,973,080,081` host (`-61.19M`, CRC final `0x382f`);
  VecZip is neutral there.
- Static-exit direct-link removes five instructions per eligible site: formal smallpt
  `-50,068,840` with 998 PCs smaller / 0 larger; inline-L2 `link_hit` falls
  `12,191,734 → 31,816` while every exit/RSB/dispatcher/region counter remains equal. CoreMark
  equal-entry is `-210,324,020`, formal c-ray is `-574,050,830`, STREAM total is `-18,740`, and
  call-dense is `-240,000,010`; every common-PC set is shrink-only.
- Default return-L1 removes RSB production/consumption when `indirect_l1` is enabled: formal
  smallpt `-45,325,251` with 881 PCs smaller / 0 larger and 99.9953% L1 hits. CoreMark equal-entry
  is `-169,232,701`, formal c-ray `-566,348,759`, STREAM `-5,480`, and call-dense
  `-352,000,012`. `SVM_INDIRECT_L1=0` call-dense is byte-identical to the old RSB path.
- Cycle-polled successor layout removes one redundant conditional-arm branch without crossing the
  per-block cold stub: formal smallpt `-203,334` with 127 PCs smaller / 0 larger. Static local branch
  bytes fall `5,732 → 5,140` while 518 cycle edges and 4,144 poll bytes remain exact. CoreMark
  equal-entry is `-51,081,278`, c-ray `-328,719`, and STREAM `-1,412`; every common-PC set is
  shrink-only.
- Paired indirect-L1 state loading shortens the production fast path from nine instructions to
  eight: formal smallpt `-1,905,795` with 376 PCs smaller / 0 larger. CoreMark equal-entry is
  `-31,825,037`, formal c-ray `-91,110,798`, STREAM `-879`, and call-dense `-64,000,000`;
  every common-PC set is shrink-only. The scale-10 call-dense wall-time median is neutral
  (`1.045108s → 1.044901s`), unlike the rejected exclusive-pair prototype.
- Live resident-FPR publication removes a full-home copy even when the producer has safe SSA uses
  after the publication: formal smallpt `-11,994,709` with 79 PCs smaller / 0 larger. Formal c-ray
  equal-entry is `-136,043,687` with 174 PCs smaller / 0 larger; STREAM is `-199` with 26 PCs
  smaller / 0 larger, while CoreMark is effectively neutral. PPM, c-ray IDAT and STREAM validation
  remain exact.
- Scalar-load FPR fusion collapses `LDR X + MOV zero + 2×INS` into one fault-site `LDR Dtarget`
  for 6,285,771 formal smallpt executions. Formal smallpt is `-18,857,313` with 64 PCs smaller /
  0 larger; formal c-ray equal-entry is `-115,072,803` with 27 PCs smaller / 0 larger; STREAM is
  `-174` with 7 PCs smaller / 0 larger and CoreMark is equal-entry neutral. Every changed static
  PC shrinks by a multiple of three; all three oracles remain exact.
- Scalar-sqrt resident publication maps a legacy scalar `VecFUnary(kind=sqrt)` result to the
  proven-dead resident merge home. Formal smallpt is `-3,111,742` (`-0.2663%`) with 20 PCs each
  exactly two instructions smaller and 0 larger. Formal c-ray equal-entry is `-1,036,470` with
  one PC two instructions smaller and 0 larger; STREAM and CoreMark are equal-entry neutral.
  PPM, c-ray IDAT, STREAM validation and CoreMark CRC remain exact.
- Legacy scalar-binary resident publication reuses a proven-dead fixed left home. Formal smallpt
  is `-2,482,103` (`-0.2130%`) with 31 PCs smaller and 0 larger. Formal c-ray equal-entry is
  `-107,640,218` with 83 PCs smaller and 0 larger; STREAM is `-1` and CoreMark is equal-entry
  neutral. The 64-bit legacy lowering computes in a reserved temporary before inserting lane0,
  so the resident high lane is never overwritten. All four oracles remain exact.
- Direct absolute-address materialization removes the scratch-to-result transport for every
  absolute `GetOperand`. Formal smallpt is `-21,753,480` (`-1.8704%`) with 794 PCs smaller and
  0 larger. Formal c-ray equal-entry is `-243,851,552` with 1,924 PCs smaller and 0 larger;
  STREAM/CoreMark are `-1,137` / `-673`, also shrink-only. All oracles and spill counts remain
  exact. The feature is now default ON; `SVM_ABS_CONST_MAT=0` selects the prior code shape.
- Compact FCMP publication combines the raw parity-byte write and AF clear into one 27-bit BFI;
  AXFLAG and lazy host NZCV remain unchanged. Formal smallpt is `-14,894,552` (`-1.3051%`)
  with 144 PCs smaller and 0 larger. Formal c-ray equal-entry is `-88,615,708` with 118 PCs
  smaller and 0 larger; STREAM/CoreMark are `-23` / `-4`, also shrink-only. All oracles and
  spill counts remain exact.
- Trailing constant `SetLocation` publication is deferred only when it is the final enabled IR
  instruction. Formal smallpt is `-30,041,304` (`-2.6671%`): 998 PCs each shrink by exactly three
  instructions and none grow. Formal c-ray equal-entry is `-344,393,514` with 4,151 PCs smaller
  and none larger; STREAM/CoreMark equal-entry are `-9,702` / `-126,194,385`. CoreMark has one
  entries=0 layout-only version grow by nine instructions and no dynamic growth. All four oracles
  and spill counts remain exact.
- Compact FCMP carrier publication writes ordered/raw-parity directly with the existing `CSET VC`
  when every consumer is a compact publish plus at most one proven-safe `FCmpCondSet`. Formal
  smallpt is `-14,894,552` (`-1.3586%`) with 144 PCs smaller and none larger. Formal c-ray
  equal-entry is `-88,615,708` with 118 PCs smaller and none larger; STREAM is `-23` across 11
  shrinking PCs. CoreMark's four FCMP PCs are consistently `-4`; repeated A/B isolates an unrelated
  one-entry cold-layout toggle at `0x4668fd`. All four oracles and spill counts remain exact.
- Semantic Nop elision removes the backend ARM `NOP` while retaining every IR decode/translate
  metadata effect. Formal smallpt is exactly `-8,991,381` (`-0.8314%`) with 245 PCs smaller and
  none larger. Formal c-ray equal-entry is `-141,118,113` with 837 PCs smaller and none larger;
  STREAM/CoreMark equal-entry are `-1,390` / `-57,725,341`, also shrink-only. Placement/alignment
  Nops use a separate code-pool path and remain unchanged. All four oracles and spill counts are exact.
- Zero-register FPR lane publication extends the existing default-on `zero_store_zr` proof to a
  single-use, unspilled integer `LoadImm(0)` consumed by `SetHostFPR`. The fixed lane reads directly
  from `wzr/xzr`; multi-use, pseudo-observed, spilled and nonzero values retain materialization.
  Formal smallpt is `-3,582,031` (`-0.3340%`) with 41 PCs smaller and none larger. Formal c-ray
  equal-entry is `-114,264,417` with 144 PCs smaller and none larger; STREAM is `-46` across 5
  shrinking PCs. CoreMark is effectively neutral and keeps CRC `0x382f`. All spill/oracle gates pass.
- Shared zero-store elision accepts multiple uses only when the complete global use count is closed
  by same-block StoreUniform, StoreMemory or SetHostFPR value operands. Formal smallpt is
  `-4,290,381` (`-0.4014%`) with 74 PCs smaller and none larger. Formal c-ray equal-entry is
  `-114,750,206` with 142 PCs smaller and none larger; STREAM is `-5` and CoreMark is neutral.
  PPM, IDAT, STREAM, CRC and spill gates remain exact.
- Direct simple `CondSet` extraction replaces `AND + MSR NZCV + CSET` with one `UBFX` for
  EQ/CS/MI/VS and `UBFX + EOR` for their inverse conditions when x26 is authoritative. Live host
  PSTATE and compound conditions retain the existing path. Formal smallpt is `-4,434,213`
  (`-0.4165%`) with 101 PCs smaller and none larger. Formal c-ray equal-entry is `-21,411,778`
  with 293 PCs smaller and none larger; STREAM/CoreMark equal-entry are `-2,168` / `-2,210`,
  also shrink-only. PPM, IDAT, STREAM, CRC and spill gates remain exact.
- Scaled immediate addressing lets direct-mode `[base + imm]` use the AArch64 unsigned scaled
  load/store encoding instead of `MOV imm + [base, register]`. Pair, shift, writeback and bounded
  bias paths retain their prior predicates. Formal smallpt is `-98,407` (`-0.0093%`) with 18 PCs
  smaller and none larger. Formal c-ray/STREAM/CoreMark equal-entry are `-93` / `-8` / `-8`, all
  shrink-only; every output and spill gate remains exact.
- Direct single-bit `TestFlags` materialization replaces `TST + CSET` on saved flags with one
  `UBFX`; when PSTATE is authoritative it uses `MRS + UBFX`, avoiding both the destructive test and
  any NZCV restore. Formal smallpt is `-7,074,834` (`-0.6674%`) with 81 PCs smaller and none larger.
  Formal c-ray equal-entry is `-32,621,346` with 221 executed PCs smaller; the only larger common
  PC has zero entries. STREAM/CoreMark equal-entry are `-1,100` / `-7,481,108`. PPM, IDAT,
  STREAM, CRC and spill gates remain exact.
- PSTATE-preserving zero tests use `CLZ + LSR` for zero and append `EOR` for nonzero instead of
  publishing guest flags and then issuing `CMP + CSET`. The path is rejected when a later local
  Goto/NotGoto/BindLabel would merge control state; the unrestricted prototype failed the existing
  zero-rotate repro. Formal smallpt is `-5,840,166` (`-0.5546%`). Formal c-ray equal-entry is
  `-21,374,140`; layout growth contributes only 278 dynamic instructions. STREAM is `-472`, while
  CoreMark's `+79,936` is a 0.0015% layout-level change with exact CRC. All output and spill gates
  remain exact.
- Live-PSTATE single-bit `TestFlags` now uses one non-clobbering `CSET` instead of `MRS + UBFX`;
  x26-backed tests keep their one-instruction `UBFX`. Formal smallpt is `-3,536,755` (`-0.3378%`)
  with 74 PCs smaller and none larger. Formal c-ray equal-entry is `-16,348,046` with 148 PCs
  smaller; the sole larger common PC has zero entries. STREAM/CoreMark equal-entry are `-30` /
  `-3,740,015`, and every output/spill gate remains exact.
- Encodable negative `GetOperand` displacements now use `SUB` directly instead of materializing
  the signed immediate and issuing `ADD`. Formal smallpt is `-781,140` (`-0.0749%`); equal-entry
  delta is `-781,195` with 99 PCs smaller and none larger. Formal c-ray equal-entry is
  `-10,527,227` with 288 PCs smaller and none larger; STREAM/CoreMark are `-214` / `-59`.
  PPM, IDAT, STREAM, CRC and spill gates remain exact.
- Direct-hash L1 tables now align their storage to the complete table span. The inline return and
  dispatcher paths can therefore replace `AND + ADD` with one `BFI` while retaining the same
  entry count and probing contract. Formal smallpt is `-1,905,795` (`-0.1828%`) with 377 PCs
  smaller and none larger; unit/version/entry counts are identical. Formal c-ray raw/equal-entry
  deltas are `-90,922,904` / `-91,098,755` with 1,129 equal-entry PCs smaller and none larger.
  STREAM raw/equal-entry are `-941` / `-875`; CoreMark raw/equal-entry are `-31,825,004` /
  `-31,825,027`. PPM, c-ray IDAT, STREAM validation, CoreMark CRC and spill gates remain exact.
- Direct-mode `[base + index]` memory operands now stay composite through the frontend instead
  of materializing an intermediate `GetOperand`; the existing ARM64 memory emitter consumes the
  register-offset form directly. Formal smallpt is `-54,841` (`-0.0053%`) with 45 PCs smaller and
  none larger. Formal c-ray raw/equal-entry are `-5,506,465` / `-5,469,864` with 200 equal-entry
  PCs smaller and none larger. STREAM equal-entry is `-111`; CoreMark equal-entry is `-640,298`
  with 66 PCs smaller and none larger. PPM, IDAT, STREAM, CRC and spill gates remain exact.
- A strict `Sub(RSP,size) -> StoreMemory -> SetHostGPR(RSP)` backend proof now emits one
  pre-index store in direct mode. The store completes before AArch64 base writeback, so a
  synchronous fault retains the pre-instruction RSP; biased memory and base/data overlap
  (`push rsp`) reject the fold. Formal smallpt is `-38,941,142` (`-3.7413%`) with 228 PCs
  smaller and none larger; unit/version/entry counts are identical. Formal c-ray raw/equal-entry
  are `-401,378,477` / `-400,778,948` with 1,108 equal-entry PCs smaller and none larger.
  STREAM raw/equal-entry are `-5,508` / `-4,228`; CoreMark raw/equal-entry are
  `-100,077,187` / `-100,077,202`. PPM, 64-spp c-ray IDAT, STREAM validation, CoreMark CRC and
  spill gates remain exact.
- Constant-address caching now groups ordinary memory operands by their 4 KiB guest page instead
  of requiring the exact same address. One allocated page base serves every scaled/unscaled
  encodable offset in the verified idle-register window; biased memory rematerializes each exact
  guest address. The corrected cache is default ON with `SVM_CONST_ADDR_CACHE=0` rollback.
  Formal smallpt is `-17,523,872` (`-1.7491%`) with 47 PCs smaller and none larger; all
  unit/version/entry counts remain identical. Formal c-ray raw/equal-entry are `-227,905,071` /
  `-227,602,716` with 67 PCs smaller and none larger. STREAM raw/equal-entry are `-152` / `-216`;
  CoreMark raw/equal-entry are `-147` / `-216`. PPM, c-ray IDAT, STREAM validation, CoreMark CRC
  and spill gates remain exact.
- The default x86 GPR map is not full pin: `SVM_X86_PIN_EXT=2` keeps 14 of 16 architectural GPRs
  resident; only opt-in level 3 adds R13/R15 and remaps R12-R15 onto x6-x9. The prior level-3 audit grew 4,400-unit host
  code by 3.89%, raised spill memory operations from 5,425 to 14,071 and lost 10.18% on loaded
  CoreMark, so it remains rejected. In the retained formal smallpt logs, 72,003,699 / 86,980,059
  weighted SetHostGPR instances and 123,911,206 / 129,472,617 GetHostGPR instances already emit
  zero bytes; the remaining emitted moves are about 14.98M / 5.56M host instructions.
- A single audited consumer may now name the same pinned W read more than once, so `test eax,eax`
  lowers directly to an `ANDS` using w22 instead of first extracting a capture. Callee-saved
  pinned byte/word/dword reads also feed `SXTB/SXTH/SXTW` directly. A later second consumer keeps
  the computed capture. Existing formal entries attribute 2,054,236 + 706,866 executions to
  the two proven hot shapes; this estimated 2,761,102-instruction reduction is not folded into the
  headline formal total because long benchmark reruns were stopped. Mac focused validation passes
  6 cases / 429 assertions; no full suite was run.
- A sole U8/U16/U32 pinned read used as the value of an ordinary `StoreMemory` now stores directly
  from its fixed W home. Address reuse, a later value use, an intervening write to that home, U64
  values and TSO stores keep the computed capture. The local Release `4 8 6` short screen has
  byte-identical PPM output and identical 3,250-PC / 3,502-version shapes; the strict common set is
  `-852,490` weighted host instructions (`-0.083737%`), with all top-20 PCs present. Its
  Mac-to-retained-Orb host coverage is only 98.362170%, below the 99.9% promotion gate, so this is
  recorded as a directional screen rather than a formal result. Focused validation passes 5 cases /
  8 assertions.
- A bounded StoreUniform census found that stores are 7.331% of the current local `4 8 6` host
  account and `ThreadContext64::carry_inverted` alone is 36.64% of them, or 2.686% of total host.
  Inverted/direct publications split 63.56%/36.44%. On FlagM hosts the decoder now keeps carry
  Direct across units: sub-family producers emit `CFINV`, add-family producers publish nothing,
  conditional carry paths merge back to Direct, and block-entry CF consumers no longer load the
  polarity byte. Non-FlagM hosts keep the old byte ABI. The temporary census output was removed.
  The short PPM stays byte-identical, but the candidate changes unit formation from 3,242 PCs /
  3,494 versions to 2,757 PCs / 3,597 versions. The strict common-PC join covers only 37.091% and
  6/20 top PCs, so the raw `host_dynamic` change (`2,245,710 -> 593,133`) is not a promotable
  weighted result. Focused IR validation passes 4 assertions; func_tests passes JIT/interpreter,
  region-off, flags-regs-off and CFINV-off with checksum `9f52b7d59285dbe5`; branch-only passes all
  six grids and helper-fault passes 38/38. No long benchmark or full suite was run.
- A five-second truncated opcode audit covers only 4.825% of the current short-run host weight, but
  separates real loads from their address tax: 2,203 / 2,231 weighted `LoadMemory` sites emit one
  instruction, and extra address formation is 46 / 2,277 load-attributed instructions (2.020%), or
  0.161% of the covered host account. The same slice exposes `InvertCarry` at 5.82%, so the larger
  reducible target is dead carry normalization rather than the `LDR` body. `a9f5ddf` lets the existing
  carry-liveness pass remove an inversion only when a later in-block C writer covers every path before
  a reader; live-out C, `TestFlags(C)`, helpers, branches and the block-wide ADC/SBB gate keep it.
  The strict local `4 8 6` A/B has identical 2,757-PC / 3,597-version sets, 100% host/entry and top-20
  coverage, byte-identical PPM, zero spills and common host `593,160 -> 591,583` (`-1,577`,
  `-0.265864%`) with no growing PC. Focused validation passes default/rollback carry elimination
  (32 / 11 assertions), canonical carry (4), SaveCV (4), simple CondSet (62), rotate-zero carry (4),
  region branch flags (46) and full NZCV publication (3). No long benchmark or full suite was run.
- Adjusted U8 `Select(condition, 1, 0)` now emits `CSET`, and its two `LoadImm` producers are
  suppressed only when every use is another proven direct select. The proof accepts only direct
  boolean producers and bounded `And` / `Or` compositions. A sole `CondSet` condition is folded into
  direct and general selects; when PSTATE is dirty, `CSET` / `CSEL` runs before `MergeNZCV` so the
  old guest-flags and pending-token publication boundary remains. Other non-local conditions retain
  the existing `MergeNZCV + CMP` path. The
  strict local `4 8 6` A/B has identical 2,757-PC / 3,597-version sets, 100% host/entry and
  top-20 coverage, byte-identical PPM, zero spills and common host `591,583 -> 583,155`
  (`-8,428`, `-1.424652%`)
  with no growing PC. General conditional-select fusion contributes `-5,652` (`-0.959907%`) on top
  of the direct form; one 2,502-entry PC accounts for 5,004 of those instructions. CondSet,
  flag-elimination and COMIS checks pass 3,588 assertions. The bounded
  setcc/cmov/jcc and BMI diagnostics are byte-for-byte identical to the pre-change failure sets;
  the 512-iteration interpreter run passes. A frontend-only SetCC collapse reached `586,558`, but
  raised BMI JIT/interpreter divergences from 324 to 327 by skipping this publication boundary and
  was fully reverted. No long benchmark or full suite was run.
- Adjacent non-float low-64/high-zero `SetHostFPR` publication now emits one `FMOV D,X` at the
  original publication point. The high zero must be a U64 constant, both stores must be
  adjacent lanes of the same resident home, and the existing fault-sensitive `LDR D` fusion keeps
  priority. A zero low value clears the complete home with `EOR V,V,V` rather than requesting the
  macro-level forbidden `FMOV D,XZR`. This recovers rejected scalar-load shapes without moving
  their load or fault point. A shared high-zero constant is suppressed only after every one of its
  uses belongs to a fused publication; any other user retains materialization. The
  strict local `4 8 6` A/B has identical 2,757-PC / 3,597-version sets, 100% host/entry and top-20
  coverage, byte-identical PPM, zero spills and common host `583,155 -> 580,841` (`-2,314`,
  `-0.396807%`) with no growing PC. Shared-zero coverage contributes `-1,601` (`-0.274877%`)
  beyond the sole-use form. FPR publication/fault, resident-XMM, scalar SSE, COMIS and
  directed/fuzzed VEX.128 validation pass 5,616 assertions. The pre-existing SSE batch-B
  JIT/interpreter divergence count remains exactly 392 on both arms. No long benchmark or full
  suite was run. A complete bounded pair census found 105 adjacent publications: all are GPR64 +
  high-zero, 43 use the existing load fusion and 62 use value fusion, for 4,004 weighted pair
  executions; no adjacent pair remains unmatched. The temporary census output was removed.
- `2634fe4` extends resident full-home publication to `VecShuffle32Indexed`. The allocation pass
  and ARM64 emitter independently restrict the same producer set; the existing live-range,
  observer, fixed-home and conflict gates remain unchanged. `TBL` and the proven `EXT` lowering
  are alias-safe when their result is allocated directly in the resident home. The strict local
  `4 8 6` A/B keeps the same 2,757-PC / 3,597-version sets, 100% host/entry and top-20 coverage,
  byte-identical PPM, zero spills and no growing PC, while common host falls `580,841 -> 580,620`
  (`-221`, `-0.038048%`). The resident-XMM matrix now includes both accepted and conflicting
  indexed-shuffle publications and passes 854 assertions; PSHUFD all-immediate coverage passes
  6,914 assertions, directed VEX.128 passes 76, and fixed-seed 256-iteration VEX.128 fuzz passes.
  The pre-existing SSE batch-B JIT/interpreter divergence count remains exactly 392. No long
  benchmark or full suite was run.
- `613dd12` lets a sole U8/U16/U32 callee-saved fixed-home read feed `Sub` directly from its W
  view. Capture reuse and an intervening fixed-home write still retain the read bridge; other
  consumers are unchanged. The strict local `4 8 6` A/B keeps all 2,757 PCs / 3,597 versions,
  100% coverage, the exact PPM and zero spills, with common host `580,620 -> 580,290` (`-330`,
  `-0.056836%`) across ten shrinking PCs and none growing. The six pinned-GPR focused cases pass
  14 assertions. Fixed-seed 256-iteration ALU and mixed fuzz retain their exact pre-existing
  88 / 106 divergence counts on both arms. A matching callee-saved `Add` extension saved only 51
  (`0.008789%`) and was removed. No long benchmark or full suite was run.
- `5f9ebac` removes the redundant preparation copy in the narrow `Sub` NZCV path when the emitted
  right operand is exactly `LSL #0`: the existing U8/U16 sign-bit alignment is folded directly
  into `SUBS`'s shifted-register operand. Other shifts, immediates, composites and `Add` keep their
  old paths. The strict `4 8 6` A/B keeps all shape, coverage, PPM and spill gates exact and reduces
  common host `580,290 -> 573,037` (`-7,253`, `-1.249892%`); ten PCs shrink by one instruction and
  none grow. Pinned-GPR focus passes 16 assertions; flags elimination, SaveCV and CondSet pass
  32 / 4 / 62. Fixed-seed ALU/mixed fuzz retain the exact baseline 88 / 106 divergences.
- `993acce` admits `SignExtend` to the existing last-use GPR publication proof. `SXTB`, `SXTH` and
  `SXTW` write a fresh alias-safe destination, while the unchanged input-live, conflict and observer
  gates decide whether it may be the fixed home. The strict screen remains same-shape and exact,
  with common host `573,037 -> 569,108` (`-3,929`, `-0.685645%`), eight shrinking PCs and none
  growing. The expanded producer matrix passes 367 assertions and pinned-GPR focus passes 16;
  fixed-seed mov/extend and mixed fuzz retain the exact baseline 98 / 106 divergences.
- `5a47163` collapses narrow logical flag identities. U8/U16 `TEST reg,reg` no longer builds a
  redundant `And`; a zero-immediate `Or` with no data observer publishes flags from its input, and
  the exact low `BitExtract -> Or(0)` chain may share the source register because the flag emitter
  discards all physical high bits. U8/U16 NZ uses one shifted `ADDS`; U32/U64 uses `TST`. A bounded
  no-detail `4 8 6` host-dump A/B has the same 556 observed PCs, 193 shrinking PCs, no growth and
  `-937` static instructions. Its exact-version intersection covers 12.038773% of the retained
  formal host weight and already removes 43,721,205 weighted instructions (`-0.221854%` of the
  complete formal total), so this is a lower bound rather than a new formal ratio. Hot unit
  `0x419287` falls `260 B -> 240 B`, removing all five instructions around `test dil,dil` beyond the
  single NZ producer. The short PPM is byte-identical at SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Two strict collector runs
  hit the 15-second hard limit and produced no records; they were not extended or retried. Logical
  shape plus flags/SaveCV/CondSet focus passes 120 assertions; fixed-seed ALU/mixed fuzz remains at
  the documented 88 / 106 existing divergences. No long benchmark or full suite was run.
- `477947d` / `e389f9d` / `5b94050` / `183bf78` / `46197f6` compact the remaining high-volume
  NZCV publication and restore mechanisms. Contiguous partial publication is
  `MRS + UBFX + BFI`; full and non-contiguous publication share one emitter across ordinary and
  region paths. `MSR NZCV, x26` now consumes the packed flags word directly because the system
  register ignores all bits outside 31:28. Published region veneers omit even that restore when
  the existing full incoming-flags kill proof succeeds, and simultaneous C/V/AF clears use one
  `BFC` over bits 26:29. No compatibility path or runtime switch was added.
- The final bounded `4 8 6` dump has 558 sections/PCs. Against the 556-PC starting dump, the large
  cold printf unit at `0x47f6c0` split into `0x47f6c0`, `0x47f8a8` and `0x47f8f0`; it has only 240
  retained-formal entries and 36,960 host weight, so it is explicitly excluded rather than treated
  as an exact version. The remaining 555 common PCs have 329 shrink, 226 unchanged, 0 growth and
  `-4,093` static instructions. The exact retained-formal subset is 541 PCs / 12.038585% coverage
  and removes 330,449,276 weighted instructions (`-1.676794%` of the complete formal total). This
  remains a conservative lower bound, not a replacement formal FEX ratio.
- Each same-shape stage was shrink-only: direct packed-register NZCV restore is `-2,068` static /
  `-179,793,723` weighted, proven-dead published-entry restore is `-673` / `-42,596,165`, and the
  contiguous C/V/AF clear is `-668` / `-52,706,706`. The short PPM remains byte-identical at
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
- Flags codegen focus passes 142 assertions in 10 cases; region/trampoline/L1 focus passes 224
  assertions in 5 cases, and the auxiliary region localization/link set passes 63 assertions in
  3 cases. Fixed-seed 256-iteration ALU/mixed fuzz remains at the documented 88 / 106 existing
  divergences. No long benchmark, stress run or full suite was run.
- FEX-aligned RE=0 same-harness refresh for formal smallpt: SVM host/guest
  `3.335622 → 3.267832`; the landed stages fold this to about `2.478306`. With unchanged FEX
  `1.549`, ratio is `2.153× → 1.600×`. The earlier
  2.180× table used a different retained unit-formation artifact, so quote the current gap as
  approximately 1.59–1.65× rather than mixing the two raw tables.
- PPM SHA-256 remains
  `fe96f7e48295b27c8df8236294052d138c3ed130b81d022739907fe6b2cde5aa`; prior equal-entry
  c-ray IDAT remains `54256cb4b3c6313a65ea12ebb7b81e30`, 64-spp formal c-ray is
  `d0c71130abf3544a86b64417bc488c21`, and STREAM validates.
- Scalar-load structure/fault tests pass 4 cases / 20 assertions and VEX.128 move differential
  passes at seed 424242 on Mac and Orb. Live-publication tests pass 3 cases / 9 assertions. Existing FPR focus passes
  10 + 794 + 90 + 27 assertions. Scalar-sqrt publication passes 2 shapes / 6 assertions; after
  legacy scalar-binary coverage the current FPR focus is 9 cases / 125 assertions on Mac and Orb.
  The 64-case NaN truth matrix passes Mac/Orb with AFP disabled and cold path 0/1. FLAGS six-grid, helper-fault 38/0,
  clone four-grid and the
  1664-unit/11-guest fingerprint all pass.
- Absolute-address ON/OFF fingerprint matches for 1664 units / 11 guests. Its three focused gates
  pass 3 + 1 + 10 assertions on Mac and Orb; the ON/OFF × function/block/interpreter six-grid is
  byte-identical with rc=101 and checksum `9f52b7d59285dbe5`.
- COMIS compact flags all-consumer differential passes 3,482 assertions on Mac and Orb. FLAGS
  0/1 × function/block/interpreter is byte-identical; the stage fingerprint remains
  1664 units / 11 guests.
- Trailing static-location fallback focus passes 39 assertions on Mac and Orb; cycle signal and
  repeated delink/relink pass 30 / 142 assertions on Orb. Fixed-seed full suite keeps the same
  191 passed / 35 existing failed cases and the same 45 failure sites. Baseline/candidate FLAGS
  0/1 × function/block/interpreter twelve-grid is byte-identical, and the fingerprint remains
  1664 units / 11 guests.
- Compact FCMP carrier all-consumer differential passes 3,482 assertions on Mac and Orb; flags
  focus passes 46 assertions on both. Fixed-seed full suite remains 191 passed / 35 existing failed
  cases with the same 45 failure sites. Baseline/candidate FLAGS 0/1 × function/block/interpreter
  twelve-grid is byte-identical, and the fingerprint remains 1664 units / 11 guests.
- Semantic Nop elision passes the x86 Nop family, RSB/indirect structure, direct-link fallback and
  flags focus on Mac/Orb with 1 / 26 / 39 / 46 assertions; COMIS passes 3,482 assertions. Fixed-seed
  full suite remains 191 passed / 35 existing failed cases with the same 45 failure sites.
  Baseline/candidate FLAGS twelve-grid is byte-identical, clone futex/lock four-grid is green, and
  the fingerprint remains 1664 units / 11 guests.
- Zero-register FPR lane publication extends the existing zero-store matrix to 405 assertions on
  Mac. FPR publication/fault focus and flags focus pass on Mac/Orb; COMIS passes 3,482 assertions
  on both. Baseline/candidate FLAGS twelve-grid and the 1664-unit/11-guest fingerprint match.
  Fixed-seed Orb remains 191 passed / 35 existing failed cases, now 44 failed assertions with no
  new failure category.
- Shared zero-store elision extends the Mac matrix to 537 assertions. Helper-fault is 38/0;
  FPR/flags focus and COMIS 3,482 assertions pass on Orb. Baseline/candidate FLAGS twelve-grid,
  zero-store OFF/ON six-grid and the 1664-unit/11-guest fingerprint are identical. Fixed-seed Orb
  remains 191 passed / 35 existing failed cases / 44 failed assertions.
- Direct simple `CondSet` structure passes 62 assertions on Mac and Orb; flags focus, SaveCV and
  COMIS pass 46 / 4 / 3,482 assertions. Baseline/candidate FLAGS twelve-grid is byte-identical,
  helper-fault is 38/0, and the 1664-unit/11-guest fingerprint matches. Repeated top-level seed
  424242 runs remain 191 passed / 35 existing failed cases; nested child seeds vary the existing
  config/fuzz assertion count between 44 and 45 without adding a failure category.
- The existing address/host-base focus passes 39 assertions on Mac and Orb. Baseline/candidate
  FLAGS twelve-grid and the 1664-unit/11-guest fingerprint are byte-identical. Fixed top-level
  seed 424242 remains 191 passed / 35 existing failed cases / 45 failed assertions.
- Flags focus passes 46 + 12 + 24 + 4 + 62 assertions on Mac and Orb; COMIS passes 3,482 on both.
  Baseline/candidate FLAGS twelve-grid, helper-fault 38/0 and the 1664-unit/11-guest fingerprint
  match. Fixed top-level seed 424242 remains 191 passed / 35 existing failed cases / 45 failed
  assertions.
- The existing zero-rotate repro passes 3 assertions on Mac and Orb. Flags focus and COMIS remain
  green, FLAGS twelve-grid, helper-fault 38/0 and the 1664-unit/11-guest fingerprint match. Fixed
  top-level seed 424242 returns to 191 passed / 35 existing failed cases / 45 failed assertions.
- Flags focus, the zero-rotate repro and COMIS remain green on Mac and Orb. Baseline/candidate
  FLAGS twelve-grid and the 1664-unit/11-guest fingerprint match; fixed top-level seed 424242
  remains 191 passed / 35 existing failed cases / 45 failed assertions.
- The address/host-base focus passes 39 assertions on Mac and Orb. Baseline/candidate FLAGS
  twelve-grid is byte-identical. The candidate is self-consistent at 1,657 units / 11 guests;
  compared with the 1,664-unit baseline, seven units coalesce and decoded-block/IR totals fall
  while every guest output remains exact. Fixed top-level seed 424242 is 191 passed / 35 existing
  failed cases / 44 failed assertions.
- The seven-instruction L1 structure gate passes 26 assertions on Mac and Orb; inline-L1
  signal/invalidation passes 13, trampoline coverage passes 154, and production direct-link
  coverage excluding the existing disk-cache failure passes 393 / 349 assertions on Mac/Orb.
  Baseline/candidate FLAGS twelve-grid is byte-identical and the 1,657-unit/11-guest fingerprint
  matches. Fixed seed 424242 is 191 passed / 35 existing failed cases / 45 failed assertions with
  the same failure-file set.
- The address-lowering focus passes 84 assertions on Mac and Orb. Baseline/candidate FLAGS
  twelve-grid is byte-identical. The candidate is self-consistent at 1,657 units / 11 guests;
  unit and decoded-block totals stay fixed while six guests lose 152 aggregate IR instructions.
  Fixed seed 424242 remains 191 passed / 35 existing failed cases / 45 failed assertions with the
  same failure-file set.
- Stack-push structure and fault recovery pass 3 cases / 13 assertions on Mac and Orb. The
  baseline/candidate FLAGS twelve-grid is byte-identical, and the function fingerprint matches at
  1,657 units / 11 guests. Excluding the three new focused cases, exact baseline/candidate suite
  runs are both 191 passed / 35 existing failed cases / 44 failed assertions with the same failure
  locations; the known nested-child variation remains 44–45.
- Constant-page cache structure passes 16 assertions on Mac and Orb, including nearby addresses,
  scratch exhaustion and biased-memory exact-address fallback. Cache OFF/ON FLAGS twelve-grid and
  bounded-bias func_tests are byte-identical; the function fingerprint matches at 1,657 units /
  11 guests. Final default-ON and rollback suite runs both return 194 passed / 35 existing failed
  cases / 45 failed assertions with the same failure locations.
- RSB/indirect structure focus passes 26 assertions, including the paired state/cache load and
  no-target dispatcher path. A temporary mismatched-return check passes default, both L1-off RSB
  frames, FLAGS-off and interpreter paths and was deleted. Mac and Orb builds pass.
- The production inline-L1 pending-signal test passes 6 assertions on Mac and Orb. Direct-link
  production passes 11 cases / 395 assertions under FLAGS=0, including cache lifecycle. Static
  SetLocation fallback and repeated delink/recompile paths pass 36/142 assertions on Mac and Orb.
- Region edge, direct-cycle signal and region-flags focus pass 42/30/46 assertions. The new layout
  matches the baseline fingerprint for 1664 function units over 11 guests.
- MT SMC stress passes 200/200 with zero host failure, lost guest or timeout. Catch/fuzz seed
  424242 gives the latest Orb tree 191 passed / 35 existing failed cases / 44 failed assertions;
  the failed case count and categories are unchanged.
- Detailed mechanism table, the rejected Linux scalar-insert prototype and exact deltas are in
  `docs/codegen-fpr-flags-refresh-2026-08-24.md`.

## Invariants (do not violate)

- Recorded cond is **guest** polarity; PSTATE is **host NZCV** (maybe CFINV). Unproven `b.cs` inverts JC/JNC.
- `RecordLocalCondition` does **not** prove PSTATE liveness. Merge is x26 publication.
- Successor `dirty && requested=={}` Merge copies **no** PSTATE. If-pack is last publication for CheckHalt/GetFlags/Unpark-from-x26.
- Empty-requested Merge must **not** copy-all PSTATE (SIGABRT).
- Do **not** force `nzcv_dirty=true` at region block entry (skip+dirty-at-entry lost density).
- Unpark reads **x26**. Transparent L2 exits on RE=1 still need a pack at HostExit or region-mode Dispatcher.
- `BranchOnlyFlags` producers must **not** set `nzcv_requested/dirty` (SIGABRT). Covering them in `SuccessorCovers` is the opposite: they **overwrite PSTATE**, so the **pred** If can skip pack.
- INC leftover C is live; do not treat INC as covering CF; do not copy C at INC entry (halt reason 2).
- `kMaxFuncBlocks=128`. `lazy_budget < 128` used to make **128 eager**. Keep `<=`. Never raise default window past 128 without raising the cap **and** keeping published-L2 skip.
- A faulting full-width `LoadMemory` may publish directly into its pinned home because the fault does not commit the destination. Partial/narrow writes and any path rejected by the existing local observer, conflict, or liveness proof must keep the real publication instruction.
- An ordinary `StoreMemory` may read a pinned W home directly only when an offset-zero U8/U16/U32
  `GetHostGPR` has exactly that store-value use and no intervening write to the home. Address uses,
  later uses, U64 values and TSO stores keep the capture instruction.
- FlagM units keep host C equal to x86 CF at every cross-block boundary. A live `InvertCarry` after
  a sub-family producer is part of that canonicalization, not a polarity toggle that can be removed
  locally. It is dead only when backward liveness proves a later in-block C write covers every path
  before a read; live-out C and any intervening reader retain both the inversion and its producer.
  Direct carry cannot use the inverted-carry `HI/LS` folding rule. Non-FlagM hosts still persist and
  load `carry_inverted`.
- A low32 copy may skip `BitExtract` only when its sole use is the immediately following
  `ZeroExtend32To64`; the wrapper must still emit a W move and keep all later uses.
- A width round trip may substitute the original U32 SSA only for
  `BitExtract(ZeroExtend32To64(v32), 0, 32)` with one ordinary same-block consumer inside the
  128-IR window.
- Same-width extraction is restricted to a U32 result and an audited W-reading consumer. U8/U16
  normally retain the real extract because backend physical high bits are not implied by the narrow
  IR type. The only narrow exception is the immediate, sole-use low extract feeding a no-data-use
  `Or(0)` flag direct: its shifted NZ producer discards every physical high bit, and both the RA
  tie and emitter shape must remain exact. Pseudo and opaque calls always retain the extract.
- A static `SetLocation` exit may emit a direct-link site only for the existing same-module,
  non-self, BlockLink-enabled region contract. The cycle poll remains before the site; unavailable
  regions, cross-module targets and disabled BlockLink keep the inline L2/dispatcher fallback.
  The site must stay registered with LinkManager so SMC can restore its trampoline branch.
- An `indirect_l1` module must not produce RSB frames. The first `State` pair is
  `exit_request + indirect_l1_code_cache`; production `ForwardIndirectL1` loads it with `LDP` and
  tests the signal bit with `TBNZ`. Its signal arm must return through the shared trampoline so the
  offset-zero `LDAR` confirms the request before returning `Signal`; profile mode keeps its separate
  cache-base load. Missing retained targets return to the dispatcher. Only L1-off modules may pair
  `EmitRSBPush` with `EmitRSBPop`.
- A direct cycle edge may use the next region block to choose conditional layout, but it is not a
  true fallthrough: retain `LDAR/CBNZ`, then branch over the source block's immediately following
  cold stubs. Falling through after the poll executes the CodeMiss/Signal stub on every iteration.
- A complete V128 producer may remain in a resident FPR home after `SetHostFPR` only when its full
  interval has no other value mapped to that home, the pre-publication observer checks pass, and no
  later write to that home precedes the producer's last use. The producer cannot be rebound to a
  second resident home; the emitter must independently reproduce all of these checks.
- A scalar memory load may replace adjacent low-load/high-zero resident publications only when
  both producers are single-use, the high value is exactly U64 zero, and the load-to-publication
  window contains no fault/helper, local-control, target-home access, or overlapping mapped FPR.
  Emit the D-register load at the original fault site and reprove the complete recipe there.
- A zero-register store value must be an unspilled integer `LoadImm(0)`. Multiple uses are allowed
  only when the inclusive global use count exactly equals same-block StoreUniform, StoreMemory or
  SetHostFPR value operands. Cross-block, address, arithmetic, pseudo, floating and nonzero uses
  retain materialization.

## Failed / do not retry

| Attempt | Result |
|---|---|
| BFXIL 2-insn Merge | Broke FLAGS=0 and ON |
| INC C-preserve / skip leftover C | Halt reason 2 |
| Empty Merge copy-all PSTATE | SIGABRT |
| Skip If + dirty-at-entry | **+3.4%** host |
| Transparent skip + pack CheckHalt/all HostExit | **7.154→7.200B** |
| Full split-arm pack / invert loop branch | hang rc=124 |
| Mark BranchOnly TEST dirty so `then_covers` fires | SIGABRT FLAGS=1 |
| Fallthrough-only defer / else-only Merge without requested | no density or hang |
| Function CFG live-out for ordinary flag deletion | Re-tested after condition-specific liveness. Bounded smallpt changes 2,802/3,435 units/versions to 2,790/3,011, covers only 98.918559% of baseline host weight and grows the common subset `395,147 -> 396,850` (`+0.430979%`); fully reverted. |
| `SVM_RA_WIDTH_CHAIN=1` | 0 on coremark |
| `GetHostGPR` 32-bit `Mov W` for callee-saved pins | 0 |
| `SVM_FUNC_LAZY=128` before `15e5165` | **31.3B** host, RE=0-shaped entries |
| Require every successor-cover to survive fault and reach AdvancePC | **6.300→6.551B** host; too conservative, reverted |
| PF/AF dedicated GPR on current CoreMark | saves 0; adds 67,754,766 dispatcher/RSB recovery instructions |
| SHA census from failing OpenSSL path | PageFatal at `rip=0x62b930` before valid hashing; no performance evidence |
| Generic same-width fold including U8/U16/CallLambda | fixed-seed U16 popcount helper mismatch; narrowed to U32 W-consumer whitelist |
| True fallthrough after a direct cycle poll | falls into the source block's cold stub; CoreMark 1.224s→57.597s despite correct CRC, fully rejected |
| Tagged L1 control word loaded with nonzero-offset `LDAR` | AArch64 `LDAR` has no immediate offset; VIXL ignored it and production hit PageFatal, fully reverted |
| `LDAXP` request/cache-base pair | smallpt `-1,905,795`, but call-dense scale-3 wall time regressed 223%; fully reverted |
| Terminal-tail cycle success branch | formal smallpt bit-identical at `1,168,614,398`; safe target-bound pool is empty after successor layout, fully reverted |
| Same-value carry-polarity publication dedup | equal-entry smallpt only `-23,703` (`-0.0022%`), with 137 PCs larger, 17 smaller and changed unit formation; fully reverted |
| Generalized legacy scalar resident-left chain | formal smallpt byte-identical at `1,072,445,284`; exact `GetHostFPR` origin is not the remaining limiter, fully reverted |
| Generated TestZero/TestNotZero local condition | FLAGS=1 transparent window still saves only 919 formal smallpt instructions; fully reverted |
| Sole TestZero/TestNotZero direct/general Select fusion | strict local `4 8 6` saves only 79 (`-0.013547%`); exact PPM and no growth, but the extra builder state is not justified and was fully reverted |
| Zero-register `SetHostGPR` publication | smallpt / c-ray equal-entry only `-1` / `-22`; existing GPR coalescing already absorbs it, fully reverted |
| Transparent `BitCast` zero-store graph | formal smallpt and c-ray are byte-identical at every equal-entry PC; the proof reaches no remaining materialization and was fully reverted |
| Pinned GPR immediate-offset memory address | smallpt is byte-identical and c-ray's partial retained-entry subset saves only `0.012823%`; the extension was fully reverted |
| Dead overwritten `SetHostGPR` IR deletion | the common smallpt subset shrinks, but unit/version formation diverges, 1,675 dynamic spills appear, and c-ray `64x48/s1` times out at eight seconds; the IR-lifetime prototype and its test were fully removed |
| Remaining absolute `GetOperand` materialization | 21.75M left-immediate instances are true two-part constants; ADRP/literal alternatives do not preserve the current relocation and mapping contract |
| Saved-flags compound `CondSet` | two-instruction HI/LS and GE/LT forms were implemented and validated, but execute 0 times in formal smallpt/CoreMark and the c-ray audit sample; GT/LE still need three inputs, so the zero-gain prototype was removed |
| General narrow `TEST` direct-`And` flags | 496-PC bounded A/B had 29 shrinking and 31 growing PCs, only 13 net static instructions and `-623` retained-formal-weighted instructions; fully reverted |
| Direct U8/U16 zero-only `CMP` | the local-kill form shrinks smallpt's common set by `1.282746%`, but CoreMark changes to 2,849 PCs / 3,382 versions and fails with `crcfinal=0x630a`; even the strict `BranchOnlyFlags(Z)` form reproduces the same CRC failure, so the emitter, test and CMake entry were fully removed |
| `SVM_FLAG_FULL_ELIM=1` after condition-specific liveness | Exact oracle and unit/version sets, but bounded smallpt grows `399,467 -> 408,275` (`+2.204938%`), dominated by partial-NZCV publication at `0x419287`; remains OFF. |
| Global inverted-carry ABI default | Re-tested after the scalar-FPR proof fix. The oracle stays exact, but bounded smallpt changes 2,802/3,435 units/versions to 3,207/3,493 and grows the strict common subset `370,990 -> 394,889` (`+6.441953%`). Hot `TEST`/logical producers make Direct carry dominant in execution even though the emitted producer census favors subtraction. Fully reverted; any remaining carry work needs explicit edge polarity. |
| IR-rewriting integer `Sub` branch-only carry normalization | the non-carry-only form still changed the bounded unit/version set from 2,755/3,621 to 2,785/3,056; strict coverage was 99.648660% with two growing PCs, below the 99.9% gate despite `-0.908475%` on the comparable subset, fully reverted. `86aaac4` is a separate backend-only EQ/NE proof and does not revive this rewrite. |
| Generic live `ZeroExtend32To64` result remap into the publication home | the residual `live_ok` census classified stores before conflict and observer checks, so its weighted total overstated the opportunity. The broad remap made many required moves merely change location, added low-view preservation copies, and halted the 1,000-iteration CoreMark screen at `0x402e60`. The RA module, logs and temporary restrictions were removed; retain only backend copy fusions that prove a net one-instruction form. |
| Pinned-copy low-32 `BitExtract` address aliases | the focused local/Orb case passed, but bounded smallpt and 1,000-iteration CoreMark were both byte-identical with 100% weighted coverage. The emitter hook, matcher extension and test were removed. |
| Multi-use fixed-home capture reuse | The exact post-publication Xor/narrow-alias graph shrank three formal CoreMark CRC blocks by two instructions each and the common subset by `9,120,008`, but changed `crcfinal` to `0x4555`. A later consumer still requires the original capture even when the visible target-home overwrite window appears closed. The matcher, diagnostics and test were fully removed. |
| Direct pinned immediate publication | The broad constant form shrank the formal common CoreMark subset by `16,240,214` but returned CRC `0x6096`. Restricting it to the audited `LoadImm(8) -> home 0` shape still shrank `13,400,033` and returned CRC `0x398e`. Writing the fixed home at the producer crosses an old-value observation not represented by the local alias whitelist; the emitter path, state and test were fully removed. |
| Disable inline indirect L1 and use the existing RSB path | The exact 2k CoreMark unit/version set and CRC remain stable, but weighted host work grows `351,368,645 -> 368,914,549` (`+4.993588%`). The current guarded RSB pop is longer than the seven-instruction inline-L1 hit path; do not flip the existing feature or re-enable RSB pushes while indirect L1 is active without a new continuation ABI. |
| Delay dynamic `current_loc` publication on the inline-L1 hit path without a new continuation ABI | SMC invalidation keeps the L1 key and replaces only its value with the shared miss trampoline. That trampoline must reload the published location before its L2 walk, and the signal/key-miss path returns through the dispatcher for the same reason. Moving the store cold therefore requires a known-register continuation ABI for every inline exit and invalidation value; omitting it locally can dispatch or compile the stale PC. |
| Omit a pinned U32 self-extension when later reads are only scaled memory indices before a full overwrite | `0x403630/0x403688` can encode their address indices with `UXTW`, but the intervening memory RMW may fault. A signal context must already observe the x86-required zeroed high 32 bits of the guest register, so the apparent `mov w22,w22` remains architecturally visible before that fault boundary. |
| Outline implicit `PCMPISTRI 0x02` EqualAny through an exact-clobber vector leaf | `__strcspn_sse42` shrinks `331 -> 265` and the exact 2,212-root SQLite shape moves `300,175 -> 300,109` with no growth. The first un-warmed pairs were mixed, but a warmed reverse-order confirmation regressed internal median `1.411 -> 1.444s` and wall median `1.696 -> 1.730s`. The self-`0x3a` variant separately grew repeated strcmp roots by six instructions and was removed before the final timing. `a04d265` was fully reverted by `84bafd8`; do not trade the existing inline EqualAny loop for a helper call. |

## Next ready (pick one, measure, revert on 124/134)

The current single-version opcode ledger covers about 82.85% of formal smallpt host execution. The
pre-canonical-carry largest per-op responsibilities were StoreUniform 82.41M, VecFMulScalar64 78.89M,
LoadMemory 76.55M, GetOperand 65.87M, VecFAddScalar64 58.03M, LoadUniform 56.33M and
StoreMemory 51.64M. Do not subtract the local short carry census from these formal values: the
candidate changes unit/version formation and must pass a future formal gate before the ledger is rebased.

The new bounded census closes redundant partial/full merge masks, redundant pre-`MSR` masks and
provably dead published-entry restores. `0a2eabf` adds the cross-unit pending-flags entry for full
NZCV edges whose target proves a complete overwrite. Remaining partial requested masks and targets
that observe incoming flags still need a broader contract; do not reopen them with more mask
peepholes.

1. **Cross-edge carry polarity** — `28f459a` closes dead-edge `CMP/Jcc` carry publication, including
   `JB/JAE/JA/JBE`, and `5bdf30e` closes condition readers that do not consume C before a later
   in-block C overwrite. Remaining inversions preserve architecturally live CF across an edge or
   serve a real carry observer. The global inverted ABI is a measured regression because hot
   logical producers are Direct. Continue only with an explicit edge/version polarity contract
   that canonicalizes mixed joins; do not change the decoder-wide default or add a runtime
   polarity store on FlagM.
2. **Pinned GPR residuals** — keep the 14-register level 2 map as the performance default. Recount actual emitted bytes,
   not GetHost/SetHost IR. The largest remaining SetHost moves in the retained log implement real
   guest copies such as `mov rbp,rdi` and `mov rbx,rdx`; deleting them requires architectural
   register renaming, not another fixed-home peephole. Direct ordinary StoreMemory payload reads
   and callee-saved `Sub` reads are closed. Continue only with another measured consumer that can
   read the fixed home directly while retaining capture, width and helper-clobber proofs; the
   same `Add` extension was only `-51` and is closed. Selective R12/R14 pinning is landed; do not extend
   the map to R13/R15 without resolving the Mac 15-register hang. The pre-R12 bounded emitted-write census has
   31,705 weighted `SetHostGPR` instructions: 13,002 are `GetHostGPR`-root guest copies. The older
   3,944 `SignExtend` pool, the signed-load publication/low-alias pool and ordinary saved-flags
   zero-test aliases are now closed. Recount roots after each landed stage; do not treat the
   remaining total as a generally safe GetHost elimination. Another 5,157 `Sub`-root live writes
   are Mac biased-memory stack updates separated from publication by a faulting store; Linux
   direct already folds the exact safe form into pre-index stores, so this is not a remaining
   FEX-alignment pool.
3. **smallpt remaining link** — covered link is now about 6.6%. Region/cycle tails are about
   2.1%; their acquire poll and branch across per-block cold stubs are load-bearing. Audit the
   roughly 1.26% remaining return-L1 static sequences separately; address formation is now one
   `BFI`, and `LDP + CMP + CSEL + BR` has no obvious base-ISA fusion. Public host exit executes
   only 139 times. Full-NZCV external direct edges with overwrite-first targets now use the pending
   entry; continue this direction only with an explicit partial-mask or observing-target ABI. The
   remaining `SetLocation` tail is dynamic or has a later observer and must not inherit the
   trailing-constant proof.
4. **Remaining FPR publication** — the older SetHostFPR, scalar64-copy and low-load/high-zero
   accounts predate both platform-neutral scalar insert and the full XMM0-15 resident ABI below.
   The current weighted ledger has 76,229 emitted `SetHostFPR` instructions. A bounded rejection
   census found that the apparent remaining scalar candidates are dominated by publication to a
   second resident home and chains whose high lanes originate in another XMM home; these are real
   guest copies, not an unclosed fixed-home tie. Fault captures remain load-bearing. Non-AFP hosts
   retain the legacy scalar high-lane preservation sequence, and resident-disabled configurations
   retain ordinary State publication. The indexed-shuffle pool remains closed; do not broadly
   whitelist scalar merge-home shapes.
5. **Remaining composite EA** — direct `[base+imm]`, `[base+index]` and matching scaled-index
   forms are now direct. Remaining computed forms involve bias/32-bit wrapping, shifts or an
   AArch64-unencodable scale; require an exact encoding and wrap proof before extending the gate.
   The truncated short audit attributes only 2.020% of observed `LoadMemory` work to address
   formation, so do not treat the raw opcode total as a removable pool.
6. **CoreMark remaining truncations** — raw BitExtract is no longer a pool. Adjacent sole-use
   narrow memory extensions now write their consumer register directly, adjacent low U8/U16
   extracts plus `ZeroExtend32` emit at most one instruction, and clean-load self-extensions emit
   none. Only reopen other 8/16-bit cases with a consumer-specific physical-high proof and the U16
   helper regression in the gate.
7. **SHA valid workload first** — fix or replace the current OpenSSL guest path that PageFatals before hashing, then redo the boundary census. Do not bypass guest fault semantics.
8. **PF/AF dedicated GPR is closed** until a new canonical park/recovery carrier yields a nonzero mechanical saving; the current audit is strictly negative.
9. **Do not** grow the default region window again for coremark (64 == 128). Other benches might still want 128 **after** the lazy fix.

## 2026-08-25 continuation

- Default level 2 now pins 14 of 16 guest GPRs. Level 3/full pin remains closed: the fixed audit
  grows move/bridge work by 3.526%, and the Mac Debug pool can abort at 6 available registers for
  a 21-register scratch demand.
- `quick_shape.py --static-only` now captures the existing host dump without runtime entry counters.
  The bounded `smallpt_wh_x64 4 8 6` screen completes in 2.3–4.2 seconds on this Mac Debug build,
  covers 558 PCs and preserves PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Use it to reject
  zero-impact or growing candidates; retained formal entries remain the promotion weights.
- Closed in the fast screen: region window 128 grows common code by 357 instructions (`+0.572473%`);
  AFP minmax and SHUFPS immediate are byte-identical; width chain saves 9 instructions; loop flags
  grows 40; full flag elimination grows 3. Direct `GetHostGPR -> SetHostGPR` is byte-identical, and
  forwarding through `ZeroExtend32To64` saves only 38 of 62,372 instructions (`0.060925%`). All
  production prototypes and their checks were removed.
- `4f27b7a` adds canonical-state cross-unit dual entries. The cold linker still returns through the
  published entry after its C++ helper call; once linked, the patched branch targets the counted
  body entry and skips the redundant `MSR NZCV,x26; B body`. Flags-transparent blocks retain the
  published entry because their terminal may republish incoming PSTATE. The second entry is carried
  through LinkManager generation/SMC ownership and disk-cache format v6.
- Each eligible linked transition now has a mechanical two-instruction reduction. Three tiny
  interleaved wall pairs were dominated by warm-up noise (the final pair was 2.219s/2.218s), so this
  stage makes no wall-time claim. The region trampoline test covers public-first/direct-after-patch,
  signal delink remains green, and the smallpt oracle is exact.
- The bounded pending-flags census found 81 full-NZCV edges in smallpt (`4 8 6`) across 51 source
  merge groups. Of 54 edges with a compiled target, 18 targets already satisfy the existing
  overwrite-before-observe/fault proof. The matching c-ray census found 263 full-NZCV edges among
  436 observed external edges. A per-target cold-stub design would have grown smallpt by 41 static
  instructions and was rejected.
- `0a2eabf` completes the full-NZCV cross-unit contract. Candidate sites initially branch around the
  three-instruction source merge and use a pending slow trampoline that materializes PSTATE NZCV
  into x26 while preserving PF/AF. A proven overwrite-first target publishes a pending entry at its
  counted body. Before an incompatible target is patched directly, LinkManager restores the source
  merge; unlinked and far arms remain safe through the pending trampoline.
- The first implementation required every conditional arm to be linked and compatible. The bounded
  smallpt/CoreMark runs enabled zero groups, so it was replaced before commit. In the final design,
  unexecuted cold arms no longer block a hot compatible arm. The smallpt debugger census observed
  five incompatible-link restores and two compatible re-enables. Each surviving linked transition
  replaces three merge instructions with one branch; the first cold link pays two extra trampoline
  instructions, and an incompatible linked edge returns to the old steady-state cost.
- Pending target entries, source patch metadata and adjusted live merge words are persisted in
  disk-cache format v7. Focused validation passes 324 assertions across seven tests, including
  conditional execution, incompatible-target fallback, signal/SMC delink, cache serialization and
  all static-pin trampoline configurations. The final static smallpt screen completed in 2.352s,
  covered 558 PCs and preserved PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Unit sizes are unchanged;
  this stage makes no wall-time claim and ran no long benchmark, stress test or full suite.
- XMM0 now joins the default resident ABI, so XMM0-11 map to v16-v27. The bounded Orb
  `smallpt_wh_x64 4 32 24` comparison has identical 2,793-PC / 3,647-version sets, 100% host,
  entry and top-20 coverage, exact PPM SHA
  `fe779f46a4c8f0f75ab42b573253492e5f1da2ee508fdb6aee62389787244cd0` and zero spills. Weighted
  host instructions fall `4,914,012 -> 4,804,966` (`-109,046`, `-2.219083%`): 163 PCs shrink,
  three grow and 2,627 are unchanged. Move-class instructions rise `1,291,180 -> 1,313,749`, but
  the fixed-home moves replace a larger uniform-state load/store cost. The final Mac static screen
  completes in 1.2-1.3s, covers 558 PCs and preserves its existing PPM oracle. Disk-cache format is
  v8 because cached code embeds the resident-XMM ABI. Focused resident-XMM, page-fault and cache
  serializer validation passes 909 assertions; the cross-process v8 cache round trip also passes.
  This stage makes no wall-time claim and ran no long benchmark, stress test or full suite.
- Replacing the level-2 R8-R11 pins with R12-R15 kept the same 12-register pressure and exact PPM,
  but the comparable retained-entry subset grew by 4,087 instructions (`+0.145456%`). The map and
  its temporary captures were removed; do not treat guest ABI lifetime alone as a pin-selection
  proof.
- A partial-NZCV direct-link prototype deferred its three-instruction merge until a compatible
  overwrite-first target linked. The bounded density census found 16 partial arms in eight shared
  merge groups, but none of their targets satisfied the existing complete-overwrite contract;
  the weighted saving upper bound was zero. The implementation, tests and census logging were
  removed rather than retaining an unused cross-unit ABI extension.
- Scalar SSE insert now follows detected FEAT_AFP on Linux as well as macOS. Orb's native NEP check
  preserved the upper 64-bit lane with `FPCR=0x6`, and the translated 64-case SSE NaN/high-lane
  truth matrix passes with scalar insert both enabled and disabled. The old Linux rejection no
  longer reproduces after the completed FPCR/AFP lifecycle work.
- The bounded Orb `smallpt_wh_x64 4 32 24` scalar-insert OFF/ON comparison has identical 2,793-PC /
  3,647-version sets, 100% host, entry and top-20 coverage, exact PPM SHA
  `fe779f46a4c8f0f75ab42b573253492e5f1da2ee508fdb6aee62389787244cd0` and zero spills. Weighted
  host instructions fall `4,804,966 -> 4,598,890` (`-206,076`, `-4.288813%`); move-class work falls
  by the same `206,076`, from `1,313,749` to `1,107,673`. Programmatic cache direct now hashes
  effective scalar-insert policy. This stage ran no long benchmark, stress test or full suite.
- With Linux scalar insert active, extending the resident ABI from XMM0-11 to XMM0-15 no longer
  creates FPR spills. The bounded Orb `smallpt_wh_x64 4 32 24` comparison retains identical
  2,793-PC / 3,647-version sets, 100% host, entry and top-20 coverage, exact PPM SHA
  `fe779f46a4c8f0f75ab42b573253492e5f1da2ee508fdb6aee62389787244cd0` and zero spills. Weighted
  host instructions fall `4,598,890 -> 4,516,623` (`-82,267`, `-1.788845%`). State sequences fall
  from 64 to one; move-class work grows `1,107,673 -> 1,214,973`, but the eliminated State traffic
  is larger. The Mac 558-PC static screen is exact and falls `62,372 -> 62,209` (`-0.261335%`).
  Cache format is v9. Resident ABI, rollback, fault, AFP and cache tests pass locally; this stage ran
  no long benchmark, stress test or full suite.
- Scalar binary chains now transfer a resident XMM home across each exact last-use edge instead of
  computing in a temporary FPR and publishing later. At `0x4023c0`, three multiply/add chains lose
  six full-vector moves and the unit falls from 377 to 371 host instructions. Applying the retained
  baseline entries to 2,785 same-version Orb PCs gives `4,379,137 -> 4,249,587` (`-129,550`,
  `-2.958345%`); the largest reductions are `0x402777` and `0x40284e` at nine instructions each.
  The single bounded production run records 2,793 PCs, 3,659 versions, 271,517 entries,
  `host_dynamic=4,276,521`, `move_dynamic=1,059,189`, zero spills and exact PPM SHA
  `fe779f46a4c8f0f75ab42b573253492e5f1da2ee508fdb6aee62389787244cd0`.
- Resident-XMM fault captures now retain the last pre-fault carrier and let register allocation
  commit it directly into the fixed home. This fixes the packed arithmetic fault case where a
  faulting RHS load previously returned stale XMM state. Its focused code remains 16 host
  instructions with `fmul v16` committed before the load and `fadd v16` after it. One smallpt
  boundary block requires one real publication instruction, costing 3,048 retained-weight
  instructions versus the unsafe candidate. Local and Orb FPR/XMM/scalar validation both pass
  1,887 assertions across 24 tests. This stage ran no long benchmark, stress test or full suite.
- Compact COMIS relations now remain local across audited MOVSD and vector moves while the backend
  verifies every intervening IR operation with the shared host-NZCV preservation proof. This
  removes flag materialization and reload around scheduled compare/move/Jcc shapes without adding
  a recovery path or configuration switch. The bounded Orb `smallpt_wh_x64 4 8 6` weighted screen
  covers 99.995707% of the retained host weight and all top-20 PCs. The comparable total falls
  `4,285,406 -> 4,218,607` (`-66,799`, `-1.558755%`): MOVSD accounts for 39,936 and the vector
  move extension another 26,863. The largest reductions are `0x402e21` at 15,360,
  `0x40274f` at 14,575 and `0x402ddb` / `0x402dfe` at 12,288 each. The exact PPM SHA is
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`; seven focused flags,
  COMIS and fault tests pass 3,585 assertions. This stage ran no long benchmark, stress test or
  full suite.
- `178138e` detects FEAT_FRINTTS on macOS and Linux and lowers scalar float-to-integer conversion
  with the same sized-round plus `FCVTZS` mechanism as FEX. Truncating forms use `FRINT32Z` or
  `FRINT64Z`; MXCSR-rounded forms use the corresponding `X` instruction against the installed
  guest FPCR. Scalar register and memory sources remain in the FPR class instead of crossing
  through a GPR first. On Orb, each of the three hot smallpt conversions falls from 68 emitted
  bytes to eight. The bounded `smallpt_wh_x64 4 8 6` screen covers 99.995707% of retained host
  weight and all top-20 PCs; the comparable total falls `4,218,607 -> 4,181,743` (`-36,864`,
  `-0.873843%`) with exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. A 256-case conversion-only
  SSE edge sweep passes. The 7,699-assertion AVX/Rosetta run reports zero conversion mismatches;
  its two existing aggregate failures contain only `VUCOMISS` flag differences. This stage ran no
  long benchmark, stress test or full suite.
- `bb197d8` keeps live host NZCV across direct `Select`/CMOV consumers. These instructions preserve
  PSTATE, so the old unconditional merge was premature; only the independently carried PF/AF token
  is committed at the select. The bounded Orb screen keeps identical PC/version sets, 99.995707%
  retained-host coverage, all top-20 PCs and exact PPM SHA. The comparable total falls
  `4,181,743 -> 4,143,628` (`-38,115`, `-0.911462%`) with no growing PC. The COMIS all-consumer
  differential passes 3,482 assertions and the focused flags units pass 66. The existing
  setcc/CMOV/Jcc fuzz has the same 124 known flag differences in baseline and candidate. This stage
  ran no long benchmark, stress test or full suite.
- `cab14f9` lowers the twelve PSHUFD controls that map exactly to one Arm64 copy, `DUP`, `EXT`,
  `ZIP` or `TRN` instruction through one shared decoder. Immediate and cached-index forms use the
  same emitter; an indexed form skips `VecLoadConst` only when every use belongs to the proven
  direct-shuffle component. The former `SVM_PSHUFD_4E_EXT` gate and its obsolete fallback were
  removed. The bounded Orb screen completes in 2.852 seconds with identical PC/version sets,
  99.995707% retained-host coverage, all top-20 PCs, no growing PC and exact PPM SHA. The comparable
  total falls `4,143,628 -> 4,111,648` (`-31,980`, `-0.771787%`); `0x436420` accounts for 31,720.
  The exclusive-use proof passes 17 assertions, the complete 256-immediate cached/uncached sweep
  passes 6,914 and the FeatureSet contract passes 158. This stage ran no long benchmark, stress
  test or full suite.
- `90dcaf6` keeps the compact FCMP ordering carrier live across `InvertCarry`. `CFINV` changes only
  host PSTATE carry and leaves the raw ordering bit in the carrier GPR intact, so the hot compare
  path writes that carrier directly instead of routing it through a temporary GPR and `BFXIL`.
  The bounded Orb screen completes in 3.604 seconds with identical 2,755-PC / 3,621-version sets,
  99.995707% retained-host coverage, all top-20 PCs, no growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total
  falls `4,111,648 -> 4,061,499` (`-50,149`, `-1.219681%`). The COMIS all-consumer JIT/interpreter
  differential passes 3,482 assertions. This stage ran no long benchmark, stress test or full
  suite.
- `7196145` extends resident scalar-FPR ownership through an intermediate publication. When a
  scalar producer already occupies the exact fixed XMM home and dies at the next scalar operation,
  the successor remains in that home; the backend reuses the existing recursive chain proof before
  suppressing either publication. The bounded Orb screen completes in 4.024 seconds with identical
  2,755-PC / 3,621-version sets, 99.995707% retained-host coverage, all top-20 PCs, no growing PC
  and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total
  falls `4,061,499 -> 4,039,995` (`-21,504`, `-0.529460%`); `0x40248e` and `0x402497` each lose
  seven instructions per entry. Ten focused scalar-FPR, differential and fault tests pass 521
  assertions locally. This stage ran no long benchmark, stress test or full suite.
- `4355474` lets adjacent scalar memory-load publications share one zero high-half constant. The
  zero is removed only when every use participates in a proven scalar publication, while each load
  still requires single use, exact adjacency, fault-safe ordering and no live fixed-home conflict.
  The bounded Orb screen completes in 2.829 seconds with identical 2,755-PC / 3,621-version sets,
  99.995707% retained-host coverage, all top-20 PCs, no growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total
  falls `4,039,995 -> 3,994,447` (`-45,548`, `-1.127427%`). The largest reductions are `0x402777`
  at 12,288, `0x4026a0` at 10,212, `0x40248e` at 9,516 and `0x402497` at 8,916. Three focused
  scalar-load, shared-zero and fault tests pass 21 assertions locally. This stage ran no long
  benchmark, stress test or full suite.
- `4ea3a66` writes unused arithmetic results directly into the resident flags token register. Flags-
  only `Add`, `Sub`, `Neg`, `Adc`, `Sbb`, `And` and `AndNot` producers target `x12` or `w12`; observed
  results and branch-only flags keep their normal allocation. The bounded Orb screen completes in
  about three seconds with identical 2,755-PC / 3,621-version sets, 99.995707% retained-host
  coverage, all top-20 PCs, no growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total
  falls `3,994,447 -> 3,910,564` (`-83,883`, `-2.099990%`). Short-run host and move-class dynamic
  counts both fall by 14,620. The largest reductions are `0x41ef65` and `0x459300` at 6,144,
  `0x47f4d0` at 5,527, `0x433700` at 4,713 and `0x47f52a` at 4,711. The focused shape test passes
  six assertions and two flags-off regression groups pass 257. A fixed-seed 256-iteration flags
  differential retains the same 95 known differences and mismatch hash in baseline and candidate.
  This stage ran no long benchmark, stress test or full suite.
- `4595fc5` keeps a full-width coalesced arithmetic result in its pinned GPR while the flags token
  remains block-local. The proof requires the exact coalesced host publication and rejects later
  physical-register reuse, hard clobbers and every later write to the same guest home. Region edges,
  pending-flags backedges and parked state still adjust to the `x12` ABI when required. The unused
  AF-in-token state was removed. The bounded Orb screen completes in 3.701 seconds with identical
  2,755-PC / 3,621-version sets, 99.995707% retained-host coverage, all top-20 PCs, no growing PC and
  exact PPM SHA `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  The comparable total falls `3,910,564 -> 3,894,028` (`-16,536`, `-0.422855%`). Six focused flags
  codegen cases pass; the broad flags filter retains the same two pre-existing 8-bit shift failures
  in baseline and candidate. This stage ran no long benchmark, stress test or full suite.
- `8723387` removes scalar SSE destination seeding when backward SSA analysis proves the upper lane
  dead. Right operands and scalar compares consume only lane zero; left operands and scalar-unary
  merge inputs propagate liveness recursively. Full stores, publications, unknown consumers and
  unaccounted pseudo uses retain the copy. The bounded Orb screen completes in 2.782 seconds with
  identical 2,755-PC / 3,621-version sets, 99.995707% retained-host coverage, all top-20 PCs, no
  growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total falls
  `3,894,028 -> 3,870,936` (`-23,092`, `-0.593011%`); `0x40248e` loses eight instructions per entry
  and `0x402497` loses seven. Eleven focused liveness, resident-FPR, tie and AFP cases pass 282
  assertions. This stage ran no long benchmark, stress test or full suite.
- `25bc815` recognizes scalar self-XOR only when both SSA inputs are identical or equivalent pure
  `BitCast` / `BitExtract` views of the same capture. Exclusive view instructions are discarded,
  and the Arm64 emitter uses one `ANDS` to materialize zero and produce logical NZCV together. The
  bounded Orb screen completes in 3.912 seconds with identical 2,755-PC / 3,621-version sets,
  99.995707% retained-host coverage, all top-30 PCs, no growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total falls
  `3,870,936 -> 3,848,132` (`-22,804`, `-0.589108%`); `0x41ef00` accounts for 12,288 and `0x45d000`
  for 4,758. The static screen has 95 shrinking PCs, none growing and `-154` instructions. Twelve
  focused direct and flags cases pass 64 assertions. A direct frontend rewrite to a shared zero
  constant was rejected because it perturbed constant CSE and register allocation, growing the
  static screen by 56 instructions. This stage ran no long benchmark, stress test or full suite.
- `cd31ca8` keeps signed scalar integer conversions in the fixed XMM home after a proven full-vector
  zero and stores resident 32-bit or 64-bit scalars with `STR S` or `STR D`. A conversion whose only
  additional consumer is `StoreMemory` also bypasses its GPR result. Calls, local control flow and
  same-home overwrites reject the direct path. `SCVTF S/D` preserves upper lanes on the tested host,
  so the preceding vector zero remains required. The bounded Orb screen completes in 3.545 seconds
  with identical 2,755-PC / 3,621-version sets, 99.995707% retained-host coverage, all top-30 PCs,
  no growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total falls
  `3,848,132 -> 3,815,145` (`-32,987`, `-0.857221%`). The largest reductions are `0x401e2a` at
  15,240, `0x4023c0` at 9,216, `0x402e30` at 3,072 and `0x412930` at 2,304. Nine local focused
  cases pass 87 assertions; the Orb conversion filter passes 58 assertions across four cases. The
  `0x47f570` POP chain was also rechecked and already uses post-index loads for every stack advance;
  its remaining state stores are not redundant RSP updates. This stage ran no long benchmark,
  stress test or full suite.
- `4a446bf` preserves encodable scalar SSE memory operands through the frontend instead of
  materializing `GetOperand`. The shared helper covers scalar arithmetic and conversion sources,
  MOVSS/MOVSD loads and stores, low-half sources and MOVHPS/MOVLPS; it reuses the existing direct-
  mode width proof and retains the computed path for biased addressing. The bounded Orb screen
  completes in 2.813 seconds with identical 2,755-PC / 3,621-version sets, 99.995707% retained-host
  coverage, all top-30 PCs, no growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total falls
  `3,815,145 -> 3,711,542` (`-103,603`, `-2.715572%`). The two largest blocks, `0x40248e` and
  `0x402497`, each remove all 16 `ADD base,#disp` address instructions and fall `123 -> 107` and
  `122 -> 106`; their weighted reductions are 25,376 and 23,776. `0x4023c0` accounts for another
  18,432 and `0x401e2a` for 15,240. Eight focused direct, biased-address, fault and scalar SSE
  cases pass 926 assertions on both local Clang and Orb GCC builds. This stage ran no long
  benchmark, stress test or full suite.
- `3e1a605` extends zero-register stores through closed `LoadImm(0) -> ZeroExtend32` and
  `ZeroExtend32To64` chains. Every use must be another proven zero-preserving width node or a
  compatible StoreUniform, StoreMemory or SetHostFPR payload; spills and any additional observer
  retain materialization. The bounded Orb screen completes in 2.753 seconds with identical
  2,755-PC / 3,621-version sets, 99.995707% retained-host coverage, all top-30 PCs, no growing PC
  and exact PPM SHA `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  The comparable total falls `3,711,542 -> 3,685,233` (`-26,309`, `-0.708843%`); `0x45d000`
  accounts for 26,169 and falls `57 -> 46`. Short host and move-class dynamic counts both fall by
  1,865. Seven focused chain, zero-store and width cases pass 654 assertions locally; the new chain
  and width cases pass 117 unique assertions on Orb. Orb's older direct zero-store matrix still has
  its three GCC-only harness failures from disassembly beyond `CurrentBufferSize` and the forced-x18
  spill setup; the new chain case itself passes. This stage ran no long benchmark, stress test or
  full suite.
- `803900d` removes the obsolete single-sided-operand normalization now that null operand sides and
  immediate-left materialization are native runtime contracts. This restores ordinary immediate
  folding and avoids synthetic `ADD #0` instructions. Scalar `GetHostFPR` producers may also target
  the final pinned GPR directly under the existing publication, alias and observer proofs. The
  bounded Orb screen completes in 2.786 seconds with identical 2,755-PC / 3,621-version sets,
  99.995707% retained-host coverage, all top-30 PCs, no growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total falls
  `3,685,233 -> 3,664,930` (`-20,303`, `-0.550929%`); `0x413f60` accounts for 11,520 and falls
  `31 -> 26`. Short host and move-class dynamic counts fall by 439 after the operand normalization
  stage. The GPR publication proof passes 381 assertions on local Clang and Orb GCC builds. This
  stage ran no long benchmark, stress test or full suite.
- `dbb1574` extends the existing branch-only edge proof to `FCmpCondSet`. When an adjacent FP compare
  feeds only a terminal Jcc and both successors overwrite incoming flags before any observation, the
  pass removes `PublishFCmpFlags`, carry normalization and the compact relation carrier. ARM64
  independently reproves a sole condition use, PSTATE-preserving interval and absence of fault or
  helper observers before branching on raw FCMP NZCV. The bounded Orb screen completes in 2.821
  seconds with identical 2,755-PC / 3,621-version sets, 99.995707% retained-host coverage, all
  top-30 PCs, no growing PC and exact PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The comparable total falls
  `3,664,930 -> 3,554,284` (`-110,646`, `-3.019048%`). `0x402777` accounts for 18,432;
  `0x402731` and `0x40274f` account for 17,490 each. Two captures have identical static shapes.
  Local Clang and Orb GCC pass 44 flag-elimination, 85 FP-branch and 3,482 COMIS differential
  assertions. This stage ran no long benchmark, stress test or full suite.
- `86aaac4` keeps dead-edge integer EQ/NE branches on the raw `SUBS` zero flag. The frontend's
  two-successor dead-flags proof is retained as transient block metadata while the marker itself is
  still removed from executable IR. ARM64 independently requires one `Sub` producer, one carry
  inversion, one local EQ/NE condition, no other flag producer, no fault/observer and a fully
  PSTATE-preserving interval. It then suppresses PF/AF publication, carry normalization, the
  polarity store and the now-obsolete backedge flags recipe. Carry-reading and compound conditions
  retain the existing path. The exact HEAD/candidate Orb `smallpt_wh_x64 4 8 6` A/B keeps the PPM
  SHA `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`, zero spills,
  99.929555% retained-host coverage, 99.947333% entry coverage and all top-30 PCs. Comparable host
  instructions fall `3,552,297 -> 3,523,261` (`-29,036`, `-0.817387%`) with no growing PC;
  `0x47f518` contributes 23,555 and `0x419250` contributes 5,004. Local and Orb integer/FP
  dead-edge tests pass 22 / 72 assertions. The fixed-seed 256-iteration Orb setcc/cmov/jcc
  differential retains the documented 95 existing mismatches. This stage ran no long benchmark,
  stress test or full suite.
- `90fcc3c` keeps PF-only logical publications out of the pending-NZCV state. They no longer emit a
  dead `TST` or leave an empty merge mask that aborts the next ordinary `Select`. The focused
  regression passes three assertions. The corrected no-R12 smallpt shape is effectively neutral
  against `86aaac4` on common PCs (`-2` short dynamic instructions) with the same PPM and zero
  spills. The fix also lets the bounded c-ray path proceed to the next independently exposed proof.
- `438c634` extends the resident scalar-FPR chain proof through a valid in-place `VecFUnary` node.
  Register allocation and backend reproving now agree on the same fixed-home chain and coalesce its
  final publication. This removes the c-ray `0x41bc20` proof abort without weakening the last-use,
  same-home or crossing-write gates. The scalar fixed-home test passes 276 assertions; c-ray
  `128x96`, four-sample output is exact in both A/B arms.
- `c1d85c6` adds R12/x6 to the default level-2 static map while leaving x7-x9 available. The exact
  fixed-baseline/candidate `smallpt_wh_x64 4 8 6` run keeps 2,802 PCs, 3,435 versions, the PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1` and zero spills; short
  dynamic host work falls `411,321 -> 406,727` (`-4,594`, `-1.116889%`). Applying retained entries
  to the prior exact comparison gives `3,523,261 -> 3,457,146` (`-66,115`, `-1.876529%`). The
  bounded c-ray static common total falls `219,512 -> 218,190` (`-1,322`, `-0.602245%`) with exact
  output. CoreMark moves in the other direction: exact dynamic host work grows
  `6,199,760,435 -> 6,215,732,448` (`+15,972,013`, `+0.257623%`) while CRC remains `0x382f`.
  This trade is retained because CoreMark is already substantially ahead of the measured FEX
  reference, while smallpt and c-ray are current deficits. Static-pin, pinned-GPR, page-fault and
  scalar-chain focused gates pass 457 assertions. This stage makes no wall-time claim and ran no
  long benchmark, stress test or full suite.
- `f8cc411` adds R14/x7 while retaining x8/x9 for allocation. Against the R12 baseline, the exact
  `smallpt_wh_x64 4 8 6` arm keeps 2,802 PCs, 3,435 versions, the same PPM and zero spills; short
  dynamic host work falls `406,727 -> 405,255` (`-1,472`, `-0.361914%`) and retained-entry work
  falls `3,299,322 -> 3,279,483` (`-19,839`, `-0.601305%`). The bounded c-ray static common total
  falls `218,130 -> 217,397` (`-733`, `-0.336038%`) with exact output. CoreMark dynamic host work
  falls `6,215,732,448 -> 6,198,767,384` (`-16,965,064`, `-0.272937%`) with CRC `0x382f`; its
  25 dynamic spill operations round to zero percent. Local and Orb static-pin, pinned-GPR and
  page-fault gates each pass 181 assertions, and the Mac short oracle completes in 3.225 seconds.
  Adding R13/x8 for a 15-register map improved all three Orb shapes but repeatedly hung the Mac
  short run at the same 517-PC boundary with an empty output after both six and eight seconds.
  Substituting R15/x8 reproduced the identical Mac boundary and timeout, confirming a register-
  pressure ceiling rather than an R13-specific mapping issue. Both candidates were fully reverted.
  This stage makes no wall-time claim and ran no long benchmark, stress test or full suite.
- `28f459a` extends the dead-edge integer branch proof from EQ/NE to `JB/JAE/JA/JBE`. The frontend
  retains the same two-successor flags-dead certificate; ARM64 independently re-proves the exact
  `Sub`, normalization and sole terminal-condition graph, maps canonical CS/CC back to raw
  subtraction polarity, and recognizes the canonical HI/LS compound graph. It then emits one raw
  `SUBS + B.cond` path without publishing PF/AF, CFINV or a condition boolean. The exact bounded
  smallpt A/B retains 2,802 PCs / 3,435 versions, zero spills and PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`; weighted host work falls
  `405,255 -> 402,318` (`-2,937`, `-0.724729%`) with no growing PC. The bounded CoreMark shape also
  retains all 2,846 PCs / 3,379 versions and falls `6,198,767,526 -> 5,987,684,381`
  (`-211,083,145`, `-3.405244%`); the standard 20,000-iteration run keeps `crcfinal=0x382f`, while
  its score is intentionally not quoted because it now finishes below CoreMark's ten-second
  validity floor. c-ray's bounded image remains exact at SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`; its startup PC overlap
  was only 99.437285%, so no c-ray density delta is claimed. Five local focused cases pass 278
  assertions, the Orb integer case passes 66, and the fixed-seed local differential remains 90/90
  existing mismatches. This stage ran no long benchmark, stress test or full suite.
- `5bdf30e` makes flag liveness condition-specific: EQ/NE consume Z, CS/CC consume C, HI/LS consume
  C/Z, and signed relations consume only their actual N/V/Z subset. This applies consistently to
  block-local elimination and the HIR fixed point. A canonicalizing CFINV is now deleted when the
  intervening condition does not read C and a later in-block C writer covers every path; real carry
  conditions retain it. The exact bounded smallpt A/B keeps 2,802 PCs / 3,435 versions, the same PPM
  and zero spills; weighted host work falls `402,466 -> 399,467` (`-2,999`, `-0.745156%`). One cold
  PC grows by three instructions while all top-30 PCs are non-growing. CoreMark keeps all 2,846 PCs /
  3,379 versions and `crcfinal=0x382f`; its weighted change is effectively neutral at
  `5,987,684,409 -> 5,987,682,134` (`-2,275`, `-0.000038%`). The deterministic c-ray `128x96`,
  four-sample oracle remains SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`. Five focused local cases
  pass 182 assertions, the fixed-seed local differential remains 90/90 existing mismatches and the
  Mac short oracle is exact. This stage ran no long benchmark, stress test or full suite.
- `c540443` collapses a pinned low-32 self-publication from `UBFX + MOV + MOV` to the one required
  architectural W self-write. The backend proof accepts only a same-pin
  `GetHostGPR(U32) -> ZeroExtend32To64 -> SetHostGPR(U64)` chain, preserves the high-half clear,
  and redirects later transparent aliases only when every alias is used as a memory address before
  any replacement write to that pin. CoreMark keeps 2,846 PCs / 3,379 versions and `crcfinal=0x382f`;
  weighted host work falls `5,987,682,134 -> 5,885,762,053` (`-101,920,081`, `-1.702163%`). The two
  10.24M-entry hotspots at `0x403630` and `0x403688` each shrink from 25 to 23 host instructions and
  from six to four moves. The bounded smallpt gate keeps 2,802 PCs / 3,435 versions, zero spills and
  the exact PPM while moving `399,467 -> 399,449` (`-18`, `-0.004506%`). The deterministic c-ray
  oracle remains SHA `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`.
  Focused pinned-GPR, integer-width and composite-memory tests pass 60 assertions. This stage ran no
  long benchmark, stress test or full suite.
- `a184ded` lets exact U32 `Or` consumers read fixed W views directly, matching the existing
  And/Xor path without extending its permissive 8/16-bit rule. CoreMark keeps 2,846 PCs / 3,379
  versions and `crcfinal=0x382f`; weighted host work falls `5,885,762,053 -> 5,868,557,589`
  (`-17,204,464`, `-0.292306%`). The `cmp_idx` hotspot at `0x402218` shrinks from 54 to 50 host
  instructions and from 30 to 26 moves. The bounded smallpt gate remains exact and moves
  `399,449 -> 399,443` (`-6`, `-0.001502%`); the deterministic c-ray oracle remains SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`. The pinned-GPR focus
  passes 24 assertions, including a narrow-width exclusion. A first broad Or prototype admitted
  8/16-bit raw W reads, changed CoreMark to 2,847 PCs / 3,380 versions and 688,212,631 entries, and
  was fully reverted before this exact-width implementation. A later narrow-load direct-pin
  publication prototype failed the 12-second CoreMark gate and was fully reverted; keeping the
  load in an ordinary temporary and coalescing only its widening copy reproduced the exact
  `5,868,557,589` incumbent shape, so that zero-effect mechanism was also removed. This stage ran
  no long benchmark, stress test or full suite.
- `7cfc990` fuses the production x86 `MOVSS` publication graph
  `LoadMemory(U32) -> ZeroExtend64 -> SetHostFPR(low) + zero-high` into one fault-exact ARM64
  `LDR S`. The proof requires unique load/extension uses, adjacent low/high stores, an exact U32
  extension, no intervening memory or target-home observer, and the existing resident-value
  lifetime exclusion. The bounded c-ray static common set falls `215,587 -> 214,915` (`-672`,
  `-0.311707%`): 78 PCs shrink, none grow, and `0x402e70` falls `947 -> 853`. Its deterministic
  `128x96`, four-sample output remains SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`. The short smallpt gate
  keeps 2,802 PCs / 3,435 versions, zero spills and the exact PPM while moving
  `399,443 -> 399,441`. Local and Orb scalar-focused gates each pass 21 cases / 1,019 assertions,
  including the real `MOVSS` page-fault path. A first unwrapped U32-load prototype had zero
  production hits; the exact IR audit exposed the missing `ZeroExtend64` bridge. Reusing x27 as a
  spill scratch caused a c-ray static-path SIGSEGV and was fully reverted. This stage ran no long
  benchmark, stress test or full suite.
- `a134c32` keeps a Linux scalar spill definition in the already reserved x18 when the immediately
  adjacent IR instruction directly consumes it. Memory, atomic, helper, x87/SSE4.2 and internal
  control-flow instructions are barriers; other pending writes still commit normally, x18 remains
  unavailable to emitter/VIXL scratch, and block exits retain the existing flush. This removes the
  exact `STR x18, spill; LDR x18, spill` pair without introducing a new register ABI. Against the
  `7cfc990` c-ray screen, the static common set falls `215,079 -> 215,044`; six PCs shrink and none
  grow, while `0x402e70` falls `853 -> 829`. Applying retained formal entries only to the bounded
  22.719356%-covered subset gives `9,560,469,880 -> 9,480,531,174` (`-79,938,706`, `-0.836138%`);
  this is not claimed as a full formal c-ray delta. The deterministic c-ray oracle remains SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`. Smallpt remains exact at
  2,802 PCs / 3,435 versions, zero spills, the same PPM and `399,441` host instructions. CoreMark
  keeps 2,846 PCs / 3,379 versions and `crcfinal=0x382f` while moving `5,868,557,589 ->
  5,868,557,584`. Local and Orb spill focuses pass four cases with 24,282 / 26,859 assertions; the
  Orb scalar focus passes 22 cases / 1,023 assertions. This stage ran no long benchmark, stress test
  or full suite.
- `e64c635` drops the deferred x18 writeback when the adjacent consumer contains every remaining
  direct and pseudo use of the spilled SSA definition. Multi-use values keep the dirty slot and the
  previous forwarding behavior; the proof changes neither its barrier set nor the scratch ABI.
  Against `a134c32`, the bounded c-ray static common set falls `214,974 -> 214,952`; three PCs
  shrink, none grow, and `0x402e70` falls `829 -> 811`. On the same explicitly partial
  22.719351%-covered retained-entry subset, host work falls `9,480,528,636 -> 9,421,515,964`
  (`-59,012,672`, `-0.622462%`), of which `0x402e70` contributes `-59,009,238`; this remains a
  bounded projection rather than a full formal claim. The deterministic c-ray oracle remains SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`. Smallpt stays exact at
  `399,441`, and CoreMark keeps 2,846 PCs / 3,379 versions plus `crcfinal=0x382f` while moving
  `5,868,557,584 -> 5,868,557,582` and 22 -> 20 dynamic spill operations. The Orb spill focus
  passes four cases / 26,861 assertions. This stage ran no long benchmark, stress test or full suite.
- `0512649` extends the existing resident-FPR memory-store proof through the exact
  `GetHostFPR(U64) -> ZeroExtend32 -> StoreMemory(U32)` chain produced by x86 `MOVSS` stores. The
  read and bridge must each have one use, the store width must remain U32, and any same-home write
  or existing opaque barrier rejects the fusion. ARM64 then emits `STR S` directly instead of
  `lane-to-GPR + W copy + STR W`. The bounded c-ray static common set falls `214,907 -> 214,749`
  (`-158`, `-0.073520%`): 34 PCs shrink and none grow. On the explicitly partial
  22.719349%-covered retained-entry subset, host work falls `9,421,515,234 -> 9,357,193,844`
  (`-64,321,390`, `-0.682707%`); `0x402e70` contributes `-45,896,074`. The deterministic c-ray
  oracle remains SHA `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`.
  Smallpt stays exact at `399,441`, and CoreMark stays at `5,868,557,582` with 2,846 PCs / 3,379
  versions and `crcfinal=0x382f`. Local and Orb scalar focuses pass 22 cases with 1,020 / 1,025
  assertions. This stage ran no long benchmark, stress test or full suite.
- `be4b705` recognizes a low-lane `VecExtract64` whose V128 source is published to a resident FPR
  before its sole U64 memory store. The exact full-width publication must precede the store; opaque
  barriers and any later non-equivalent write to that home reject the recipe. The extract is then
  removed and ARM64 stores the resident D register directly. Against `0512649`, the bounded c-ray
  static common set falls `214,155 -> 214,138`: 14 PCs shrink, none grow. On the explicitly partial
  22.719321%-covered retained-entry subset, host work falls `9,357,174,698 -> 9,345,606,001`
  (`-11,568,697`, `-0.123635%`), led by `tform_point`, `tform_vector`, `intersectSphere` and
  `tform_vector_transpose`. The deterministic c-ray oracle remains SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`. Smallpt keeps 2,802 PCs /
  3,435 versions, zero spills and the exact PPM while moving `399,441 -> 399,439`; CoreMark remains
  `5,868,557,582` with `crcfinal=0x382f`. The low64 focus passes five assertions on Mac and Orb;
  the scalar focus passes 1,020 / 1,025 assertions. A broader direct-resident-read variant added no
  c-ray static reduction and was removed. This stage ran no long benchmark, stress test or full
  suite.
- `02a5e77` removes a sole-use simple `GetOperand` address copy when its source is an exact full-64
  pinned `GetHostGPR` mapping. The proof accepts only direct `LoadMemory`/`StoreMemory` consumers,
  rejects any intervening write to the pin, and rejects caller-saved-pin helper boundaries. The
  memory emitter then reads the fixed X register directly; ordinary SSA addresses retain the
  existing RA lifetime tie. Smallpt keeps 2,802 PCs / 3,435 versions, zero spills and the exact PPM
  while moving `399,439 -> 396,873` (`-2,566`, `-0.642401%`); `0x4192f0` contributes `-2,502`.
  CoreMark keeps 2,846 PCs / 3,379 versions and `crcfinal=0x382f` while moving
  `5,868,557,582 -> 5,849,153,108` (`-19,404,474`, `-0.330652%`). The bounded c-ray static common
  set falls `214,517 -> 214,381`: 101 PCs shrink and none grow. On the explicitly partial
  22.719336%-covered retained-entry subset, host work falls `9,345,619,486 -> 9,334,177,681`
  (`-11,441,805`, `-0.122430%`). The deterministic c-ray oracle remains SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`. Local and Orb pinned/page-
  fault focuses pass 52 + 21 assertions. A broader live pinned-publication prototype reduced the
  covered c-ray shape but made even `16x12/s1` time out; it was fully removed after confirming the
  committed baseline completes in one second. This stage ran no long benchmark, stress test or
  full suite.
- `42abcdb` fuses an ordinary logical producer's adjacent `ClearFlags(CVAF)` and
  `SaveFlags(NZ|PF)` publication. It publishes the parity token before borrowing scratch, then
  replaces the separate four-bit clear plus two-bit NZ merge with one six-bit extract/insert. The
  proof rejects region-internal, backedge, dead-edge and branch-only flag recipes, so their existing
  lazy cross-edge contracts remain untouched. Smallpt keeps all 2,802 PCs / 3,435 versions, zero
  spills and the exact PPM while moving `396,873 -> 394,264` (`-2,609`, `-0.657389%`); every
  changed equal-entry PC shrinks. CoreMark keeps all 2,846 PCs / 3,379 versions and
  `crcfinal=0x382f` while moving `5,849,153,108 -> 5,775,449,473` (`-73,703,635`,
  `-1.260074%`); its equal-entry weighted comparison is `5,910,113,189 -> 5,836,409,554`
  (`-1.247077%`). The bounded c-ray static common set moves `214,621 -> 212,641` across
  99.888300% host coverage, and the explicitly partial 22.719351%-covered retained-entry subset
  moves `9,334,183,694 -> 9,282,105,342` (`-52,078,352`, `-0.557932%`). The deterministic c-ray
  oracle remains SHA `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`.
  The new focused case passes three assertions on Mac and Orb. The broader flags/page-fault focus
  retains its two incumbent failures: the stale default-OFF assertion and the immediate-shift case
  reproduced with the new matcher disabled. This stage ran no long benchmark, stress test or full
  suite.
- `86b104f` omits an actually emitted pinned-GPR publication when the next access to that home is a
  complete overwrite. Unlike the rejected IR-deletion prototype, it leaves IR use counts, register
  allocation and unit formation untouched. The proof rejects a target read, partial rewrite,
  fault/helper, uniform-address barrier, `SetLocation` and local control; a later coalesced rewrite
  must also have a publication root after the omitted store. Smallpt keeps all 2,802 PCs / 3,435
  versions, zero spills and the exact PPM while moving `394,264 -> 391,408` (`-2,856`,
  `-0.724388%`); the equal-entry comparison is `394,282 -> 391,426` (`-0.724355%`), with every
  changed PC smaller and `0x41928c` contributing `-2,502`. CoreMark keeps all 2,846 PCs / 3,379
  versions, its existing 20 dynamic spills and `crcfinal=0x382f` while moving
  `5,775,449,473 -> 5,697,687,040` (`-77,762,433`, `-1.346431%`). The bounded c-ray static candidate
  completes in 0.936 seconds and the deterministic oracle remains SHA
  `89ccd2e15dba67378197d05524a6223795f8b8ab2d11f4d40deaef4af9f35c6e`; no c-ray density delta is
  claimed because the build-only builder-disabled control was not a valid runtime baseline. Local
  and Orb pinned/page-fault focuses pass 75 assertions across 13 cases. This stage ran no long
  benchmark, stress test or full suite.
- The Orb phase-c mirror must be synchronized with the tracked checkout using the checksum command
  below before every A/B. A partial source copy left the x86 frontend's AFP detection stale while
  rebuilding newer runtime objects; that mixed binary incorrectly restored the legacy scalar-SSE
  lane moves. The exact tracked HEAD with `SVM_X86_PIN_EXT=3` gives the calibrated smallpt baseline
  used for the next stage: 2,802 PCs / 3,435 versions, `385,558` dynamic host instructions, 288
  dynamic spill operations and the canonical PPM SHA
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
- `3e47a16` lets a U32 producer retain its flags token in a fixed guest
  GPR through the exact `ZeroExtend32To64 -> coalesced full SetHostGPR` publication. The wrapper and
  producer must share the physical register; any later same-home write, fixed clobber or unrelated
  overlapping SSA definition rejects the proof. This removes the otherwise required
  `mov w12, wPin` without changing the fault or exit publication ABI. Against the exact tracked
  HEAD, smallpt keeps 2,802 PCs / 3,435 versions and the canonical PPM while moving `385,558 ->
  385,479`; its 100%-covered equal-entry comparison is `386,584 -> 386,505` (`-79`,
  `-0.020435%`). CoreMark keeps 2,846 PCs / 3,379 versions and `crcfinal=0x382f` while moving
  `5,823,593,568 -> 5,813,471,244`; the 100%-covered equal-entry comparison is
  `5,884,553,649 -> 5,874,431,325` (`-10,122,324`, `-0.172015%`). Local and Orb flags focuses pass
  41 assertions across seven cases; pinned/page-fault focuses pass 75 assertions across 13 cases.
  A broader fixed-home publication remap grew smallpt `+4.736618%`; a generic x12 result-placement
  pass did not improve the dominant regions. Both implementations and every diagnostic path were
  removed. This stage ran no long benchmark, stress test or full suite.
- `942a63d` generalizes the pinned low-32 self-write builder into a cross-pin copy builder. An exact
  `GetHostGPR(U32) -> ZeroExtend32To64 -> SetHostGPR(U64)` chain now reads directly from the source
  home and emits one `mov wTarget, wSource`; any later alias must be a post-publication `BitCast`
  used only as a memory address. Source or target rewrites before publication, target rewrites while
  an alias is live, caller-saved helper clobbers, faults and observers reject the recipe. The focused
  module was renamed to `translator_pinned_gpr_copy.cpp`; no compatibility path or diagnostic gate
  remains. Against `3e47a16`, bounded smallpt keeps 2,802 PCs / 3,435 versions, 288 dynamic spills
  and the canonical PPM while moving `385,479 -> 385,025`; the 100%-covered weighted comparison is
  `386,505 -> 386,051` (`-454`, `-0.117463%`) with no growing PC. CoreMark keeps 2,846 PCs / 3,379
  versions and `crcfinal=0x382f` while moving `5,813,471,244 -> 5,799,708,766`; the 100%-covered
  weighted comparison is `5,874,431,325 -> 5,860,668,847` (`-13,762,478`, `-0.234278%`). Local and
  Orb pinned/page-fault focuses pass 79 assertions across 14 cases. The rejected generic RA carrier
  was removed after its short correctness and code-shape screen; this stage ran no stress test or
  full suite.
- `14e48c1` extends the same copy transaction through an optional single-use `ZeroExtend32` fed by
  a pinned U8/U16 read. The inner and outer extensions emit nothing; the publication emits exactly
  one `UXTB` or `UXTH` from the source home to the target home. The existing source/target rewrite,
  alias, fault and caller-saved-helper proofs are unchanged and the emitter replays the source
  width. Against `942a63d`, bounded smallpt keeps 2,802 PCs / 3,435 versions, 288 dynamic spills and
  the canonical PPM while moving `385,025 -> 385,023`; the 100%-covered weighted comparison is
  `386,051 -> 386,049` (`-2`, `-0.000518%`). CoreMark keeps 2,846 PCs / 3,379 versions and
  `crcfinal=0x382f` while moving `5,799,708,766 -> 5,793,328,760`; the 100%-covered weighted
  comparison is `5,860,668,847 -> 5,854,288,841` (`-6,380,006`, `-0.108861%`) with every changed
  PC smaller. Local and Orb pinned/page-fault focuses pass 83 assertions across 15 cases. The
  zero-reach low-32 address-alias extension was removed before this stage; no stress test or full
  suite ran.
- `4cc2c1e` extends the fixed-home publication proof to an exact
  `LoadMemory(U8/U16/U32) -> optional ZeroExtend32 -> ZeroExtend32To64 -> SetHostGPR(U64)` chain.
  The faulting load writes the final pinned W home directly, while the redundant extensions and
  publication emit nothing. The load remains the only instruction that can change the target
  before publication, and the existing alias, observer, target-rewrite and helper-clobber checks
  remain fail-closed. The recipe represents a memory producer by the absence of a source GPR rather
  than a sentinel register or compatibility path. Against `14e48c1` with
  `SVM_X86_PIN_EXT=3`, bounded smallpt keeps 2,802 PCs / 3,435 versions, 288 dynamic spills and the
  canonical PPM while moving `385,023 -> 384,618`; the 100%-covered weighted comparison is
  `386,049 -> 385,644` (`-405`, `-0.104909%`). CoreMark keeps 2,846 PCs / 3,379 versions and
  `crcfinal=0x382f` while moving `5,793,328,760 -> 5,788,086,168`; the 100%-covered weighted
  comparison is `5,854,288,841 -> 5,849,046,249` (`-5,242,592`, `-0.089551%`). Local and Orb
  pinned/page-fault focuses pass 69 assertions across 15 cases. A signed narrow-copy extension was
  byte-identical on bounded smallpt and CoreMark and was removed. This stage ran no stress test or
  full suite.
- `e2bb2c7` removes the final narrow-result `LSR` from U8/U16 Add, Sub and Neg when their pseudo
  flags are branch-only and `GetUses()` proves that the arithmetic value has no ordinary consumer.
  The aligned `ADDS` or `SUBS` has already produced the exact requested NZCV, and `LSR` does not
  change those flags; any SetHost, memory or other value use keeps the existing truncation. No IR
  rewrite, feature switch or fallback path was added. Against `4cc2c1e` with
  `SVM_X86_PIN_EXT=3`, bounded smallpt keeps 2,802 PCs / 3,435 versions, 288 dynamic spills and the
  canonical PPM while moving `384,618 -> 382,069`; the 100%-covered weighted comparison is
  `385,644 -> 383,095` (`-2,549`, `-0.660972%`) with every changed PC smaller. CoreMark keeps
  2,846 PCs / 3,379 versions and `crcfinal=0x382f` while moving
  `5,788,086,168 -> 5,750,483,860`; the 100%-covered weighted comparison is
  `5,849,046,249 -> 5,811,443,941` (`-37,602,308`, `-0.642879%`) with every changed PC smaller.
  Local and Orb flags focuses pass 122 assertions across four cases. Direct U64-load publication
  and signed-load publication prototypes had zero shared-shape delta on bounded full-pin smallpt
  and CoreMark and were removed completely. This stage ran no stress test or full suite.
- `5922091` specializes the existing dead-edge integer branch proof when its producer is a dead
  U8/U16 `Sub`, its right operand is a single-use encodable `LoadImm`, and the branch needs exactly
  ZF. The block recipe suppresses the immediate materialization before emission, then emits
  `SUB wResult, wLeft, #imm; TST wResult, #width-mask`; carry, signed and observed-result shapes
  retain the existing aligned `SUBS` path. The emitter replays the complete recipe and direct pinned
  source proof. Against `e2bb2c7` with `SVM_X86_PIN_EXT=3`, bounded smallpt keeps 2,802 PCs / 3,435
  versions, 288 dynamic spills and the canonical PPM while moving `382,069 -> 379,546`; the
  100%-covered weighted comparison is `383,095 -> 380,572` (`-2,523`, `-0.658583%`) with every
  changed PC smaller. CoreMark keeps 2,846 PCs / 3,379 versions and `crcfinal=0x382f` while moving
  `5,750,483,860 -> 5,750,481,642`; its 100%-covered weighted comparison is
  `5,811,443,941 -> 5,811,441,723` (`-2,218`, `-0.000038%`) with every changed PC smaller. Local
  and Orb flags focuses pass 144 assertions across four cases. An earlier emitter-only ZF
  prototype had zero net code-shape change because it could not suppress the pre-emitted
  `LoadImm`; it was removed before this block recipe. This stage ran no stress test or full suite.
- `3f079f9` generalizes the same block recipe across every condition currently accepted by the
  dead-edge proof: exact ZF, CF, or ZF+CF. The result-free U8/U16 compare now emits
  `UXTB/UXTH wResult, wLeft; CMP wResult, #imm`; this preserves the host-C inverse-borrow
  convention used by the existing raw EQ/NE/CC/CS/HI/LS branches and still suppresses the
  single-use `LoadImm`. Other flag sets and unencodable immediates remain on the aligned `SUBS`
  path. Against `5922091` with `SVM_X86_PIN_EXT=3`, bounded smallpt keeps 2,802 PCs / 3,435
  versions, 288 dynamic spills and the canonical PPM while moving `379,546 -> 379,525`; the
  100%-covered weighted comparison is `380,572 -> 380,551` (`-21`, `-0.005518%`) with no growing
  PC. CoreMark keeps 2,846 PCs / 3,379 versions and `crcfinal=0x382f` while moving
  `5,750,481,642 -> 5,724,721,561`; its 100%-covered weighted comparison is
  `5,811,441,723 -> 5,785,681,642` (`-25,760,081`, `-0.443265%`) with every changed PC smaller.
  Local and Orb flags focuses pass 152 assertions across four cases. This stage ran no stress test
  or full suite.
- `a627d94` removes an adjacent single-use low U8/U16 extraction before narrow Add/Sub/Neg flag
  alignment. The alignment `LSL` consumes the original source because it already discards every
  high bit; source/result aliasing is reproved against the original SSA, and shared or non-adjacent
  extracts retain materialization. Against `3f079f9`, full-pin smallpt keeps all 2,802 PCs / 3,435
  versions, 288 dynamic spills and the canonical PPM while moving `379,525 -> 376,993`; the
  100%-covered weighted comparison is `380,551 -> 378,019` (`-2,532`, `-0.665351%`) with no growing
  PC. Applying the retained CoreMark 20k entries to the complete short shape gives `-6,482,313`
  (`-0.112041%`) at 99.999998565% host coverage. Mac and Orb focused checks pass 117 assertions
  across six cases.
- `f42b7cc` extends integer width canonicalization through an exact 32-to-64 `SignExtend`. A low-32
  extract of that value is replaced with the original 32-bit SSA, after which the existing DCE
  removes both extraction copies. Against `a627d94`, smallpt remains exact and moves `376,993 ->
  374,499`; weighted host moves `378,019 -> 375,525` (`-2,494`, `-0.659755%`) entirely at
  `0x41a818`, where `SXTW; LSR #0; LSR #0; ANDS` becomes `SXTW; ANDS`. CoreMark changes only by
  `-2,194` on the retained-entry join. The width focus passes 68 assertions on Mac and Orb.
- `9fb39ef` makes the flags result token match its sole deferred responsibility: preserving a low
  byte for live parity. NZCV-only and AF-only producers no longer capture or publish that token;
  narrow arithmetic retains its final `LSR` only for an ordinary value use, live PF, or live AF.
  Against `f42b7cc`, smallpt moves `374,499 -> 368,937`; weighted host moves `375,525 -> 369,963`
  (`-5,562`, `-1.481126%`) with all 2,802 PCs / 3,435 versions and no growing PC. The retained
  CoreMark entry join gives `-27,184,518` (`-0.470386%`). The exact promoted 20k candidate, including
  the preceding two stages, is `5,724,721,561 -> 5,691,052,536` with CRC `0x382f`. FLAGS `0/1` x
  function/block/interpreter returns the same checksum and output SHA in all six cells. Focused
  parity, branch and width checks pass 185 assertions on both hosts; the broad `*flags*` filter
  retains only the two documented incumbent failures.
- `b5936ac` closes the largest remaining CoreMark mechanism. Function branch-only analysis may
  now cross one adjacent `InvertCarry` only when a single U8/U16 `Sub` compares against a direct
  memory RHS, both successors overwrite flags before any read, and the terminal condition does
  not read carry. The irrelevant normalization is deleted and the producer becomes a raw
  BranchOnlyFlags source. The `cmp r12w,[rdx+2] + jne` loop stops publishing full flags and stops
  forming a second hot unit version. Exact CoreMark 20k moves `5,691,052,536 -> 4,206,692,536`
  (`-1,484,360,000`, `-26.082346%`), versions `3,379 -> 3,378`, and keeps CRC `0x382f`. Current
  smallpt is byte-for-byte unchanged at `368,937`, with all 2,802 PCs / 3,435 versions, 288 spills
  and the canonical PPM. The acceptance proof has a dedicated HIR function test; branch-only,
  dead-edge and narrow-result focuses pass 120 assertions on Mac and Orb, and the FLAGS six-grid
  remains checksum-identical. A broad `SVM_FLAG_FULL_ELIM=1` retry grew smallpt by 2.376% and made
  short CoreMark abort; a later memory-left/immediate-right expansion saved only 0.000022% on the
  common CoreMark shape. Both prototypes were removed completely.
- `162a4d8` extends the fixed-home publication transaction to a spilled U32 `Add` whose complete
  value graph is one `ZeroExtend32To64` publication plus post-publication U32 Add/Sub uses or exact
  low-32 aliases. The producer writes the pinned W home directly, its wrapper and publication emit
  nothing, and later proven ALU uses read that home instead of the spill slot. The proof requires a
  genuinely spilled producer with no pseudo flags, closes every producer/wrapper/alias use, rejects
  target rewrites and caller-saved helper clobbers through the final use, and is independently
  replayed before emission. In CoreMark's two matrix loops this replaces the x18 add, spill store,
  spill reload and pinned move, plus later spill reloads, with one `ADD wPin`. Exact 20k host work moves
  `4,206,692,536 -> 4,103,652,536` (`-103,040,000`, `-2.449430%`); dynamic spill operations move
  `388,800,030 -> 38,880,030` (`-90%`) and CRC remains `0x382f`. The 100%-covered 2k shape join is
  `426,914,119 -> 416,610,119` (`-2.413600%`) with six shrinking PCs and no growth. Smallpt is
  exactly unchanged at 2,802 PCs / 3,435 versions, `368,937` raw host instructions, 288 spills and
  the canonical PPM. A forced-MEM publication test verifies `ADD wPinned` with no x18 publication
  move; pinned/read/spill focuses pass on Mac and Orb, and the FLAGS six-grid remains checksum-
  identical. The temporary rejection diagnostic reused `SVM_DUMP_IR` and was removed before
  delivery; no new switch or fallback remains.
- `e959074` composes the adjacent low-extract input recipe with the dead narrow immediate branch
  recipe instead of treating them as mutually exclusive. The specialized U8/U16 compare now resolves
  the original source before choosing a pinned W view, and the redundant `BitExtract` emits
  nothing. CoreMark's `sub edx,0x30; cmp dl,9; jbe` unit at `0x4034b0` shrinks by one instruction.
  Exact 20k host work moves `4,103,652,536 -> 4,077,892,443` (`-25,760,093`, `-0.627736%`) with
  CRC `0x382f`; the 100%-covered 2k join is `416,610,119 -> 414,034,026` (`-0.618346%`) with no
  growing PC. Smallpt moves `368,937 -> 368,902`, and its weighted join moves `369,963 -> 369,928`
  (`-35`, `-0.009460%`) with all shape, spill and PPM gates exact. A dedicated branch/extract test
  and the existing dead-edge matrix pass on Mac and Orb.
- `f8cc6ed` extends the same fixed-home publication transaction through an exact low U8/U16/U32
  alias used only by a branch-only `Or(alias, 0)` zero test. The load still writes the pinned W
  home directly, and the logical-flags emitter reads that home instead of materializing an
  intervening W copy. The existing complete-use, publication-order, target-rewrite and helper-
  clobber proofs remain fail-closed. CoreMark's byte-load zero-test unit at `0x403320` shrinks by
  one instruction. Exact 20k host work moves `4,077,892,443 -> 4,057,412,434` (`-20,480,009`,
  `-0.502220%`) with 2,846 PCs / 3,378 versions, 38,880,030 dynamic spills and CRC `0x382f`; the
  100%-covered 2k join is `414,034,026 -> 411,986,017` (`-0.494648%`) with no growing PC.
  Smallpt moves `368,902 -> 368,884`, and its weighted join moves `369,928 -> 369,910` (`-18`,
  `-0.004866%`) with all 2,802 PCs / 3,435 versions, 288 spills and the canonical PPM unchanged.
  Pinned-read, spilled-add and dead-narrow-branch focuses pass on Mac and Orb; no diagnostic or
  compatibility path was added.
- `ea82571` keeps one structured effective address across the load and store halves of a basic
  integer Add/Sub memory RMW when the address is `base + index * access_size + displacement` in
  direct mode. The single instruction therefore emits the displacement adjustment once while
  preserving the existing composite register-offset load/store encoding. LOCK/atomic operations,
  segment overrides, unindexed addresses, mismatched scales and biased memory retain their exact
  paths. CoreMark's mirrored units at `0x403630` and `0x403688` each shrink from 23 to 22 host
  instructions; no other PC changes. An exact same-build 20k A/B moves raw host work
  `4,035,105,879 -> 4,014,625,879` (`-20,480,000`, `-0.507546%`) and the 100%-covered weighted
  comparison moves `4,096,065,960 -> 4,075,585,960` (`-0.499992%`) with CRC `0x382f`. The 2k
  screen is `403,663,362 -> 401,615,362`; its weighted comparison is
  `409,759,443 -> 407,711,443` (`-0.499805%`). Smallpt remains exact at 2,802 PCs / 3,435 versions,
  `374,896` raw and `374,914` weighted host instructions with the canonical PPM. Mac and Orb
  effective-address focuses pass 29 assertions across two cases. A broad all-RMW prototype was
  rejected after it reshaped hot function allocation and introduced growing PCs; it was fully
  removed before delivery, and no diagnostic or feature switch remains.
- `b734438` extends the bounded successor-prefix proof through one static direct call. The caller
  prefix and callee entry may contain only audited flag-transparent instructions before a complete
  overwrite; reads of incoming flags, partial writers, indirect or nested calls, other control
  flow and unknown instructions reject the proof. Logical TEST/AND/OR/XOR metadata now counts its
  architectural CF/OF clears as writes, and CET ENDBR is treated as the existing semantic Nop.
  Every accepted callee prefix is stored as a guest-code dependency and registered with SMC under
  the owning caller translation. A callee-entry write therefore invalidates the caller as well;
  disk JIT cache mode conservatively rejects this proof until that dependency is serialized.
  CoreMark's mirrored `cmp byte [ptr],0; jne call` loops stop publishing full flags and collapse
  the associated hot versions. Exact 20k raw host work moves
  `4,014,625,879 -> 3,735,236,874` (`-279,389,005`, `-6.959279%`), units/versions move
  `2,846/3,378 -> 2,820/2,915`, and CRC remains `0x382f`. The 2k screen moves
  `401,615,362 -> 373,668,357` (`-27,947,005`, `-6.958649%`). Because unit formation changes,
  neither comparison is presented as a strict common-PC join. Smallpt keeps the canonical PPM
  while moving raw host work `374,896 -> 361,960` (`-12,936`, `-3.450557%`) and
  units/versions `2,802/3,435 -> 2,792/3,052`. Mac and Orb dead-edge focuses pass 174 assertions
  across three cases; direct-call dependency and non-stress SMC focuses also pass on both hosts.
  FLAGS `0/1` x function/block/interpreter returns rc 101, checksum `9f52b7d59285dbe5` and one
  identical output SHA in all six cells. No temporary diagnostic, new environment switch, stress
  run or full suite remains in this stage.
- `38cea6e` carries the architectural zero-extension guarantee of an exact U8/U16 `LoadMemory`
  into the existing dead narrow immediate branch lowering. `LDRB/LDRH` already defines a clean W
  value, so its terminal compare now reads that register directly instead of repeating
  `UXTB/UXTH`; non-load inputs retain the explicit truncation. CoreMark's `0x403630` and
  `0x403688` units each shrink by one instruction with no unit/version change. Exact 20k raw and
  weighted host work both fall by `20,480,048`: raw
  `3,735,236,874 -> 3,714,756,826` and weighted
  `3,735,236,889 -> 3,714,756,841` (`-0.548293%`), with 100% coverage, no growing PC and CRC
  `0x382f`. The 2k screen moves `373,668,357 -> 371,620,309` (`-0.548092%`). Smallpt keeps all
  2,792 PCs / 3,052 versions and the canonical PPM while moving raw `361,960 -> 361,711` and
  weighted `361,978 -> 361,729` (`-0.068792%`). Narrow-immediate and dead-edge integer focuses
  pass 105 assertions across three cases on Mac and Orb; no new switch or fallback was added.
- `d5e45c7` extends the pinned memory-address proof through an already coalesced full-width
  `LoadMemory(U64) -> SetHostGPR(pin)` publication and one exact post-publication `BitCast` alias.
  The producer use graph must close over that publication and alias, the publication must already
  be allocator-proven on the target fixed home, the address must have one memory use, and target
  rewrites or caller-saved helper barriers before that use reject the recipe. The later GetOperand
  therefore names the pinned home directly instead of emitting `mov xTmp,xPin`; the original
  faulting load and publication ordering are unchanged. Exact 20k CoreMark raw host work moves
  `3,714,756,826 -> 3,666,996,824`, and the 100%-covered weighted comparison moves
  `3,714,756,841 -> 3,666,996,839` (`-47,760,002`, `-1.285683%`) with no growing PC and CRC
  `0x382f`. The 2k screen moves `371,620,309 -> 366,844,307` (`-4,776,002`, `-1.285183%`).
  Units/versions remain 2,820 / 2,915. Smallpt remains exact at 2,792 PCs / 3,052 versions and the
  canonical PPM while moving raw `361,711 -> 361,709` and weighted `361,729 -> 361,727`.
  Existing pinned-GPR publication/address coverage passes 30 assertions across ten cases on Mac
  and Orb; no temporary diagnostic, feature switch or compatibility path remains.
- `7194669` extends fixed-home publication through an allocator-coalesced signed U8/U16 memory
  load. The load emits `LDRSB/LDRSH` directly into the published home and suppresses the separate
  narrow sign extension, outer low-32 publication wrapper and `SetHostGPR`. A post-publication
  low-32 alias may remain on that home only when every use is an audited `SignExtend` or left-hand
  U32 `Mul`; the proof closes the complete use graph and rejects target rewrites or caller-saved
  helper clobbers before the last use. Exact 20k CoreMark raw host work moves
  `3,666,996,824 -> 3,625,676,818`, and the 100%-covered weighted comparison moves
  `3,666,996,839 -> 3,625,676,833` (`-41,320,006`, `-1.126808%`) with no growing PC and CRC
  `0x382f`. The 2k screen moves `366,844,307 -> 362,712,301`. Units/versions remain
  2,820 / 2,915. Smallpt keeps all 2,792 PCs / 3,052 versions and the canonical PPM while moving
  raw `361,709 -> 361,697` and weighted `361,727 -> 361,715`. Orb pinned-GPR coverage passes
  73 assertions across 17 cases, and the signed-load multi-consumer case passes six assertions.
  The promoted 20k run completes in 3.476 seconds; no stress run, full suite, diagnostic or new
  environment switch remains.
- `c87a2a1` lets the existing exact narrow-load publication proof keep a low-width zero-test alias
  on the pinned home when the result has no SSA use and its only semantic output is an ordinary
  `SaveFlags`. This uses the same audited `Or(value, 0)` emitter path as `BranchOnlyFlags`; value
  consumers, nonzero operands and other operations remain rejected. Exact 20k CoreMark raw host
  work moves `3,625,676,818 -> 3,586,796,806`, and the 100%-covered weighted comparison moves
  `3,625,676,833 -> 3,586,796,821` (`-38,880,012`, `-1.072352%`) with no growing PC and CRC
  `0x382f`. The 2k screen moves `362,712,301 -> 358,824,289`. Units/versions remain
  2,820 / 2,915. `0x4033bb` alone shrinks from ten to nine host instructions for 19.36M weighted
  entries, and five other executed CoreMark PCs shrink by one. Smallpt keeps all 2,792 PCs / 3,052
  versions and the canonical PPM while moving raw `361,697 -> 361,682` and weighted
  `361,715 -> 361,700`. Orb pinned-load coverage passes 11 assertions across three cases and the
  broader pinned-GPR group passes 30 assertions across ten cases. The promoted 20k run completes
  in 3.242 seconds; no stress run, full suite, diagnostic or new environment switch remains.
- `9d299aa` lets an exact U8/U16 `LoadMemory` write a sole adjacent `SignExtend` or
  `ZeroExtend32` result register directly even when linear scan assigned different registers.
  Shared-register and pinned-publication paths remain unchanged; spilled consumers reject the
  direct path, and pre/post-index loads reject it when the destination overlaps the writeback base.
  Exact 20k CoreMark raw host work moves `3,586,796,806 -> 3,552,714,571`, and the 100%-covered
  weighted comparison moves `3,586,796,821 -> 3,552,714,586` (`-34,082,235`, `-0.950214%`) with
  no growing PC and CRC `0x382f`. The 2k screen moves `358,824,289 -> 355,415,880`.
  Units/versions remain 2,820 / 2,915. `0x402218` shrinks from 47 to 45 host instructions; the six
  executed matrix blocks each shrink by one. Smallpt keeps all 2,792 PCs / 3,052 versions, raw
  `361,682` and weighted `361,700` host work, plus the canonical PPM. The forced non-shared
  load/consumer case passes four assertions; pinned-load and address-liveness groups pass 11 and
  six assertions. The promoted 20k run completes in 3.112 seconds; no stress run, full suite,
  diagnostic or new environment switch remains.
- `53333fc` folds an exact adjacent `BitExtract(0, 8/16) -> ZeroExtend32` pair into one
  `UXTB/UXTH` at the extension destination. The extract must have one ordinary and raw use; pinned,
  flags-input, width-chain, low-copy and scalar-direct owners reject the recipe. Exact 20k CoreMark
  raw host work moves `3,552,714,571 -> 3,529,052,310`, and the 100%-covered weighted comparison
  moves `3,552,714,586 -> 3,529,052,325` (`-23,662,261`, `-0.666033%`) with no growing PC and CRC
  `0x382f`. The 2k screen moves `355,415,880 -> 353,049,445`. Units/versions remain
  2,820 / 2,915. `0x402218` shrinks from 45 to 43 host instructions; both main CRC loops and seven
  related blocks each shrink by one. Smallpt keeps all 2,792 PCs / 3,052 versions and the canonical
  PPM while moving raw `361,682 -> 361,466` and weighted `361,700 -> 361,484`. The two focused
  narrow-extension cases pass four and three assertions, and related narrow-flags coverage passes
  six assertions across two cases. The promoted 20k run completes in 3.236 seconds; no stress run,
  full suite, diagnostic or new environment switch remains.
- `a548ece` removes that final `UXTB/UXTH` when the extension destination already equals its source
  register and a bounded proof traces the value through only `BitCast`, `ZeroExtend32` and
  `ZeroExtend32To64` nodes to a same-width-or-narrower `LoadMemory` or `LoadUniform`. Arithmetic and
  fixed-home reads remain excluded. Exact 20k CoreMark raw host work moves
  `3,529,052,310 -> 3,520,730,080`, and the 100%-covered weighted comparison moves
  `3,529,052,325 -> 3,520,730,095` (`-8,322,230`, `-0.235821%`) with no growing PC and CRC
  `0x382f`. The 2k screen moves `353,049,445 -> 352,217,041`; `0x402218` shrinks from 43 to 41
  host instructions. Units/versions remain 2,820 / 2,915. Smallpt remains exact at 2,792 PCs /
  3,052 versions, raw `361,466`, weighted `361,484` and the canonical PPM. The clean-load
  self-extension case passes four assertions. The promoted 20k run completes in 3.183 seconds; no
  stress run, full suite, diagnostic or new environment switch remains.
- `4661866` keeps an exact `BitExtract(0, 8/16)` store payload on the fixed GPR home of an existing
  allocator-coalesced `SetHostGPR` publication. The extract must have one exact `StoreMemory` use,
  the publication must precede it and independently reprove, and a same-home rewrite or
  caller-saved helper clobber before the store rejects the recipe. `STRB/STRH` therefore reads the
  published W register directly without materializing `UXTB/UXTH`. Exact 20k CoreMark raw host work
  moves `3,520,730,080 -> 3,512,247,843`, and the 100%-covered weighted comparison moves
  `3,520,730,095 -> 3,512,247,858` (`-8,482,237`, `-0.240923%`) with no growing PC and CRC
  `0x382f`. The 2k screen moves `352,217,041 -> 351,368,630`; `0x402218` shrinks from 41 to 39
  host instructions, and two related hot blocks each shrink by one. Units/versions remain
  2,820 / 2,915. Smallpt keeps all 2,792 PCs / 3,052 versions and the canonical PPM while moving raw
  `361,466 -> 361,457` and weighted `361,484 -> 361,475`. Narrow and pinned-load coverage passes
  270 and 11 assertions on Mac and Orb. The promoted 20k and smallpt runs complete in 3.264 and
  2.431 seconds; no stress run, full suite, diagnostic or new environment switch remains.
- `8c8b26d` removes the per-site cold `RET` from production inline-L1 indirect exits. `TST` retains
  the Signal bit result while the cache address and entry load are formed; `CCMP` then accepts the
  cache tag only when no Signal is pending, so `CSEL + BR` selects either the hit entry or the
  unchanged trampoline continuation. The hot hit remains seven instructions, Signal still returns
  through the trampoline, and low-bit SMC requests retain their previous behavior. Exact 20k
  CoreMark raw host work moves `3,512,247,843 -> 3,480,422,816`, and the 100%-covered weighted
  comparison moves `3,512,247,858 -> 3,480,422,901` (`-31,824,957`, `-0.906114%`) with no growing
  PC and CRC `0x382f`. The 2k screen moves weighted `351,368,645 -> 348,185,623`; `0x403383`
  shrinks from 11 to 10 host instructions. Units/versions remain 2,820 / 2,915. Smallpt keeps all
  2,792 PCs / 3,052 versions and the canonical PPM while moving raw `361,457 -> 359,291` and
  weighted `361,475 -> 359,309` (`-0.599212%`). Static shape and pending-Signal coverage pass 28
  and six assertions on Mac and Orb. The promoted 20k and smallpt runs complete in 3.289 and 2.369
  seconds; no stress run, full suite, diagnostic or new environment switch remains.
- `673fbe1` extends the existing pinned low-alias proof from U32 arithmetic to exact same-width
  U8/U16 `ADD/SUB`. A zero-extended narrow load can now publish directly into its fixed GPR home,
  and a later low alias consumes that W register while the proof still rejects intervening target
  writes, helper clobbers and unrecognized consumers. CoreMark's `movzbl (%rdx), %edx; cmp %dx,
  %r14w` block at `0x402814` drops both the publication `MOV` and alias `UXTH`, shrinking 15 to 13
  host instructions. The exact 20k raw total moves `3,480,422,816 -> 3,476,032,828`; the matched
  weighted comparison covers `99.994908%` of entries and moves `3,480,317,008 -> 3,476,036,977`
  (`-4,280,031`, `-0.122978%`) with no growing common PC and CRC `0x382f`. The candidate compile
  set is deterministic at 2,761 PCs / 2,864 versions; its exact weighted total is `3,476,032,843`.
  Smallpt retains the canonical PPM. Static fixed-home coverage passes six assertions and a real
  JIT execution test checks 256 narrow compare inputs plus ZF/CF/SF/OF/PF-derived conditions on Mac
  and Orb. A bounded fixed-seed ALU A/B produced the identical set of 73 pre-existing mismatches in
  both binaries, so it was used only as a candidate-delta audit rather than claimed as a passing
  suite. The promoted 20k and smallpt runs complete in 3.055 and 2.361 seconds; no stress run,
  diagnostic or new environment switch remains.
- `4655e75` lets an adjacent same-width U8/U16 flags-producing `ADD/SUB` read fixed x6-x9 directly.
  The proof requires the pinned read and arithmetic to be adjacent, the producer to request host
  NZCV, and the value to have one exact consumer; ordinary narrow arithmetic and capture-breaking
  writes retain the computed path. CoreMark's two dominant U16 comparisons at `0x402814` and
  `0x402668` each lose `UBFX + UXTH`, shrinking 13 to 11 and 10 to eight host instructions. Exact
  20k raw host work moves `3,476,032,828 -> 3,467,792,816`, and the 100%-covered weighted comparison
  moves `3,476,032,843 -> 3,467,792,831` (`-8,240,012`, `-0.237052%`) with no growing PC and CRC
  `0x382f`. Units/versions remain 2,761 / 2,864. Smallpt keeps all 2,733 PCs / 2,985 versions and
  the canonical PPM while moving raw `227,552 -> 227,538` and weighted `227,570 -> 227,556`.
  New fixed-home static/runtime coverage passes four and two assertions, while pinned and narrow
  groups pass 83 and 282 assertions on Mac and Orb.
  The promoted 20k and smallpt runs complete in 3.823 and 3.199 seconds; no stress run, diagnostic
  or new environment switch remains.
- `f2b20c3` lets the decoder's successor-flags proof follow one direct unconditional jump, sharing
  the existing one-transfer budget with direct calls. The jump and proved target span are registered
  as one conservative SMC dependency; indirect or second transfers, wrapping ranges, flag reads and
  unknown instructions still reject the proof. This exposes the already-existing region PFAF
  sinking path for jump veneers such as CoreMark `0x402821 -> 0x402673`, removing hot full-flags
  publication from several loops. The exact 20k retained-weight comparison keeps all 2,761 PCs /
  2,864 versions and moves `3,467,792,831 -> 3,384,479,634` (`-83,313,197`, `-2.402485%`) with no
  growing PC and CRC `0x382f`. `0x402593` shrinks 10 to six, `0x403360` and `0x4033ec` six to three,
  and `0x402814` 11 to eight host instructions. Smallpt matches `99.990001%` of entries and moves
  `227,528 -> 226,213` (`-0.577951%`) with no common growth and the canonical PPM. Dead-edge,
  narrow and production region groups pass 180, 282 and 46 assertions on Mac and Orb. A fixed-seed
  JCC differential A/B produced the same 25 pre-existing mismatch sequences with identical SHA in
  both builds, so it was used only as a candidate-delta audit. The promoted CoreMark and smallpt
  runs complete in 3.289 and 2.386 seconds; no stress run, diagnostic or new environment switch
  remains.
- `1c24adb` extends the narrow-extract analysis to the strict unique-use chain
  `BitExtract(0,width) -> ZeroExtend32 -> LsrImm`. When the three nodes are adjacent and the shift
  remains within U8/U16 width, the extract and extension emit nothing and `LsrImm` emits one `UBFX`
  from the original value. CoreMark's CRC inner loops therefore lose both `UXTB/UXTH + LSR` pairs.
  The exact 20k comparison keeps all 2,761 PCs / 2,864 versions and moves
  `3,508,879,677 -> 3,485,519,660` (`-23,360,017`, `-0.665740%`) with no growing PC and CRC
  `0x382f`. The `0x403980/0x403990`, `0x4038d0/0x4038e0/0x403908/0x403930` and
  `0x403870/0x403880` pairs each shrink by two instructions. Smallpt keeps all 2,730 PCs / 2,998
  versions and the canonical PPM while moving weighted `226,354 -> 226,348`. The focused U8/U16
  shape passes four assertions, and the narrow group passes 286 assertions on Mac and Orb. The
  promoted CoreMark and smallpt runs complete in 4.211 and 3.027 seconds; no stress run, diagnostic
  or new environment switch remains.
- `74ab5c7` lets a sole low U8/U16 `BitExtract` feed `AND` from the original W value when the other
  operand is a constant with every bit above the narrow width clear. The mask already removes those
  bits, so the extract emits nothing; masks with any high bit set retain the old path. CoreMark's
  CRC loops lose the redundant `UXTH` before `AND 0xa001`. The exact 20k comparison keeps all 2,761
  PCs / 2,864 versions and moves `3,485,519,660 -> 3,473,279,625` (`-12,240,035`, `-0.351168%`)
  with no growing PC and CRC `0x382f`. The main CRC loop pairs each shrink by one instruction, and
  `0x402278` shares the same proof. Smallpt keeps all 2,730 PCs / 2,998 versions and the canonical
  PPM while moving weighted `226,348 -> 226,317`. Positive/rejection coverage passes three
  assertions, and the narrow group passes 289 assertions on Mac and Orb. The promoted CoreMark and
  smallpt runs complete in 3.186 and 2.285 seconds; no stress run, diagnostic or new environment
  switch remains.
- `b969f7d` lets a proved dead-edge EQ/NE self-test branch directly on the value's fixed GPR home.
  The flags pass hands the narrow proof to the backend only for `BranchOnlyFlags(AND value,value)`,
  and the backend requires an exact full-width publication, the same allocated register, and no
  intervening write to that home before it removes the logical result and emits `CBZ/CBNZ`. The
  exact CoreMark comparison keeps all 2,761 PCs / 2,864 versions and moves weighted
  `3,473,279,625 -> 3,469,019,615` (`-4,260,010`, `-0.122651%`) with no growing PC and CRC `0x382f`.
  `0x402683` and `0x4026b0` each lose one host instruction; the retained W67 census also applies the
  same reduction to the 416.7M-entry `0x402808` loop. Smallpt keeps all 2,730 PCs / 2,998 versions,
  the canonical PPM, and moves weighted `226,317 -> 226,306`. The focused fixed-home shape and full
  dead-edge group pass 4 and 184 assertions on Mac and Orb. The post-refactor 2k screen completes in
  2.394 seconds with 2,761 PCs / 2,864 versions and `346,944,290` dynamic host instructions; no
  stress run, debug path or new environment switch remains.
- `32cd3ed` folds branch-only U8/U16 equality comparisons when at least one operand is an exact
  zero-extending narrow load. Because the branch requests only Z, the backend may commute the
  operands and compare the loaded W value against the other register with `UXTB/UXTH`; other flags,
  region PF/AF preservation, shifted operands and non-load pairs retain the general alignment path.
  CoreMark's `0x402668` loop changes from `LDRH; LSL; SUBS` to `LDRH; CMP ..., UXTH`. Applying the
  formal candidate entries to the prior shape covers `99.999998%` of host weight and moves
  `3,469,019,672 -> 3,466,959,672` (`-2,060,000`, `-0.059383%`) with no growing PC and CRC `0x382f`.
  `0x402668` contributes `-2,040,000` and `0x402745` contributes `-20,000`; both shrink by one host
  instruction. The focused shape passes eight assertions and the dead-edge / narrow groups pass
  184 / 297 assertions on Mac and Orb. The promoted CoreMark and smallpt gates complete in 3.152
  and 2.308 seconds; smallpt retains all 2,730 PCs / 2,998 versions and the canonical PPM. No stress
  run, debug path or new environment switch remains.
- `1a69a59` folds the direct-mode sequence `load [base+1]; add base,1` when the base is a fixed
  GPR, the load and update are its only ordinary uses, the update flags are dead, the full update
  publishes back to the same home, and the intervening window has no fault, helper or base
  observer. The load emits the original scalar access with AArch64 pre-index writeback and the
  separate Add/publication emit nothing. A fault does not commit writeback, so the guest base still
  reflects the x86 state before its following Add. The exact CoreMark comparison keeps all 2,761
  PCs / 2,864 versions and moves `3,466,959,740 -> 3,441,199,737` (`-25,760,003`,
  `-0.743014%`) with 100% coverage, no growing PC and CRC `0x382f`. `0x4033bb` contributes
  `-19,360,000`, `0x4033d8` contributes `-5,440,000`, and `0x4033dc` contributes `-960,000`;
  each shrinks by one instruction. The focused code-shape / fault cases pass 2 / 5 assertions and
  the pinned group passes 90 assertions on Mac and Orb. CoreMark and smallpt complete in 3.357 and
  2.353 seconds; smallpt remains byte-identical at 2,730 PCs / 2,998 versions with the canonical
  PPM. No stress run, debug path or new environment switch remains.
- `2829a8d` replaces Runtime's inline 64-frame RSB storage with a 4 MiB usable mapping bracketed by
  inaccessible host pages. The stack starts at the midpoint so either call overflow or unmatched
  return growth reaches a guard. Runtime fault recovery claims the address only after the fault PC
  resolves to the current JIT and x25 is adjacent to that Runtime's guard, resets x25 to the empty
  midpoint in `ucontext`, and retries the interrupted instruction. The existing explicit RSB bounds
  remain for this infrastructure stage, so default code shape and benchmark totals do not change.
  Real `STP` lower-guard and `LDP` upper-guard recovery passes five assertions on Mac and Orb; the
  existing guest PageFatal case also passes five assertions on both. No stress run, diagnostic or
  environment switch was added.
- `52441a2` consumes the guarded RSB substrate and attacks the two remaining concentrated boundary
  costs. RSB push/pop no longer loads or compares explicit bottom/top pointers; empty frames are
  rejected by their zero dispatch slot, while real lower/upper escapes use guarded-fault recovery.
  Function translation now groups repeated dynamic terminal targets by physical register, suppresses
  eager `current_loc` stores, and branches misses to one cold publisher per register. Separately,
  fixed-home copy ownership may transfer selected post-publication U32 Add/Sub/And/Or/Xor consumers
  to the destination home after the source home is overwritten. The proof remains fail-closed for
  target rewrites, helper clobbers, observable operations, non-U32 values and other consumers.
  Against the guarded-stack baseline, exact CoreMark 20k moves `3,438,479,676 -> 3,416,879,675`
  (`-21,600,001`, `-0.628185%`) with all 2,761 PCs / 2,864 versions, 100% coverage, top-20 20/20 and
  `crcfinal=0x382f`. The retained W67 join including deferred terminal publication moves
  `13,188,392,770 -> 12,999,112,545` (`-1.435203%`) at 99.997866% coverage with no growing PC.
  Calibrated smallpt remains byte-identical at 2,730 PCs / 2,998 versions, zero spills and
  `226,048` weighted host instructions. Mac and Orb pinned-GPR, guarded-return and direct-link
  focuses pass; no stress run, diagnostic or environment switch remains.
- `46a2a23` adds a fail-closed narrow carry-chain fusion and lowers the shared terminal-publisher
  threshold from three same-register sites to two. The carry proof accepts only adjacent
  `TestFlags(C) -> Add(0,C) -> Add(value,carry)` chains whose final flags are dead, whose result is
  published only at U8/U16 width, and whose carry is still live in host PSTATE. It emits one `ADC`
  and removes the computed zero/carry value chain; all other users, flag observers and
  clobbers reject the recipe. Two deferred terminal sites exactly replace their two eager stores with
  one two-instruction cold publisher, so total code size does not grow. Exact CoreMark 20k moves
  `3,416,879,675 -> 3,397,918,414` (`-18,961,261`, `-0.554929%`) with all 2,761 PCs / 2,864
  versions, 100% coverage, top-20 20/20, no growing PC and `crcfinal=0x382f`. `0x4026ca` shrinks
  `25 -> 21`, contributing `-14,640,000`; `0x402218` shrinks `38 -> 37`, contributing
  `-4,161,115`. Calibrated smallpt stays byte-identical and moves `226,048 -> 225,561`
  (`-0.215441%`). Mac and Orb narrow/carry, pinned-GPR and direct-link focuses pass 297 / 45 / 90 /
  about 900k assertions. No long run, stress run, diagnostic or environment switch remains.
- The current FEX gap is now refreshed from a live same-input 2k run rather than extrapolated from
  the earlier retained denominator. `/usr/local/fex-measure/FEX` is the `f2e35f3` measurement build;
  both engines use the same CoreMark ELF, arguments and CRC `0x4983`, with FEX
  `FEX_HOSTFEATURES=disableavx`, multiblock enabled and code caching disabled. The join covers
  99.999934% of entries and 99.999902% of SVM host weight. Before this stage it measured SVM
  `2.004542` versus FEX `1.860013` host instructions per guest instruction, or `1.077703x`.
  `46a2a23` moves SVM to `1.993418`, or **`1.071723x`**, leaving a current same-input gap of about
  **7.17%**. This supersedes the earlier optimistic retained-table `1.052299x` estimate.
  The next concentrated weighted gaps are dynamic return continuation at `0x403383` (~9.78M),
  indirect-call boundary work at `0x402580` (~7.53M), cross-call flags publication at
  `0x403680/0x403552` (~5.80M/~5.30M), and the remaining narrow compare/ADC work at `0x4026ca`
  (~4.49M). The first three require continuation or cross-unit flags ABI work; do not replace them
  with per-site cold growth or ABI assumptions about guest code.
- Static `SetLocation(imm) + ReturnToDispatch` edges now carry the same `DirectLinkFlagsBypass`
  recipe as `LinkBlock` edges (`aa4ed1b`). This closes the missing connection on direct calls: when
  the linked target publishes a pending-flags entry, LinkManager replaces the first instruction of
  the three-instruction full NZCV merge with a branch over the whole merge and links the site to
  that entry. Incompatible publication and SMC invalidation continue to restore the original
  instruction through the existing generation/unlink transaction. There is no new runtime switch,
  fallback protocol or static code growth. An exact detached-`e53e193` 20k A/B keeps all 2,819 PCs /
  2,934 versions, 100% coverage and `crcfinal=0x382f`; the static weighted number is intentionally
  identical because the saving is a runtime patch. The retained host dump contains two dominant
  `merge; poll; BL` opportunities at `0x403320` and `0x403552`, with a mechanical linked-path upper
  bound of 65,600,000 fewer executed instructions; realization is conditional on their targets
  advertising pending-flags entries. The bounded smallpt oracle remains
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  Mac and Orb end-to-end checks pass 14 assertions for the new static-forward path and 40 for the
  existing shared conditional bypass, including actual patching to the pending-flags entry and SMC
  restoration. No diagnostic path or environment switch was retained.
- The first raw host-continuation RSB prototype was rejected and fully removed before `aa4ed1b`.
  It stored `{guest return, ADR continuation}` in the existing guarded x25 stack, reset the logical
  stack before leaving the active QSBR epoch, and sent misses to the existing deferred dispatcher
  publisher. It was correct on CoreMark/smallpt and the Mac/Orb SMC checks, but an exact
  detached-`e53e193` CoreMark comparison was `3,940,481,890 -> 4,013,934,489`
  (`+73,452,599`, `+1.864051%`) with 100% PC/version coverage. The dominant costs were two extra
  instructions at direct/indirect calls and one extra instruction at return; the older
  `build-master` comparison that looked strongly positive was stale and is invalid. Do not retry
  caller-side `ADR+STP`. A viable continuation ABI must form LR through a call-kind `BL`, push it in
  a call-entry veneer without re-materializing the guest return, and replace the per-return signal
  poll with an equally strong fault/cycle safepoint before it can beat inline L1.
- `940dcfe` moves inline indirect-exit interrupt observation to a 4 MiB aligned inaccessible L1
  mapping. Signal publication switches the Runtime's L1 base to that mapping; the exact fault-site
  metadata either enters the shared deferred-location publisher or returns through the committed
  host boundary. The hot return/indirect hit loses one instruction. Exact CoreMark 2k moves
  `339,833,893 -> 336,642,810` (`-3,191,083`, `-0.939013%`); Mac/Orb interrupt, SMC and cache
  serializer focuses pass without a stress run or retained diagnostic path.
- `736d04d` replaces the remaining dead U8/U16 subtract-and-carry chain with a fail-closed narrow
  compare. It accepts only canonical zero-extended inputs when C is the sole surviving flag and all
  extra flags are either overwritten or consumed only by the proved branch. Exact CoreMark 2k moves
  `336,649,767 -> 335,551,767` (`-1,098,000`, `-0.326155%`), entirely from `0x4026ca: 21 -> 18`.
  Narrow focuses pass 297 assertions on Mac and Orb; bounded smallpt retains its canonical PPM.
- `631e0a7` replaces the rejected caller-side continuation prototype with a generation-safe call
  ABI. Function HIR records a call-return layout root without adding an SSA/dominance edge; linear
  scan assigns the guest return to x14 when the ABI is eligible. A call-kind direct-link site stays
  `BL` through first traversal, patching and SMC unlink, while the callee entry stores
  `{x14, x30}` in the guarded x25 stack. Canonical and pending-flags call entries are published
  separately, so the existing cross-call flags bypass remains generation checked. All generated
  host exits use a region return veneer while continuation is active, and return/call misses are
  grouped by target physical register in the unit cold area. Indirect calls use a separately
  invalidated call-L1 table; Signal switches both L1 bases to the same inaccessible mapping and SMC
  clears both tables. Against a detached `736d04d` Release build, the 5-6 second CoreMark 2k short
  run covers 99.999495% of baseline host weight and moves the common weighted total
  `389,812,375 -> 388,290,607` (`-1,521,768`, `-0.390385%`); the largest return block is
  `0x403383: 10 -> 8`. The discarded inline-miss candidate was `+3.998337%` and is not retained.
  Mac direct-link/guarded-return/indirect-L1/function focuses pass 1,212 assertions, Orb passes 843,
  and 20k CoreMark returns `crcfinal=0x382f` on both. `quick_shape.py` full-unit collection no longer
  activates constant-address audit logging, keeping the Release screen bounded. No stress run,
  temporary environment switch or debug path remains. Applying the three measured post-refresh
  reductions to the last live FEX denominator estimates the remaining CoreMark host-instruction gap
  at about 5.4%; refresh both engines before treating that estimate as a new formal ratio.
- `0783986` removes the remaining concentrated indirect-call and fixed-home width costs. The
  call-L1 base now lives directly in `State`, so an indirect call no longer loads the profile
  interface first; signal publication switches this state slot to the existing inaccessible guard
  table. Call-return terminals always defer `current_loc` to their cold physical-register publisher.
  Separately, an exact same-home `GetHostGPR(U32) -> ZeroExtend32To64 -> SetHostGPR` may keep its
  post-publication U64 address aliases in the pinned register. Composite `GetOperand` aliases are
  accepted only when every result use is an ordinary memory address, and the materializer now
  observes the same pinned-value map as direct operands. Against the detached pre-stage Release
  shape, the 5.4-second CoreMark 2k comparison keeps all 3,631 PCs / 5,001 versions, 100% host and
  entry coverage, and moves `369,759,990 -> 364,387,280` (`-5,372,710`, `-1.453026%`) with no
  growth. `0x403630` and `0x403688` each shrink `19 -> 17`, while `0x402580` shrinks `25 -> 23`.
  CoreMark 20k returns `crcfinal=0x382f` on Mac and Orb. Mac/Orb pinned-GPR, direct-link without
  stress, guarded-return, function and indirect-L1 focuses pass 93/93, 1,113/771, 5/5, 252/252
  and 13/13 assertions. The bounded Orb smallpt screen completes in 4.949 seconds with zero spills;
  no stress run, diagnostic, temporary environment switch or check remains.
- The requested five-item tranche is complete. Static-call flags bypass has generation-checked
  pending-flags entries (`aa4ed1b`, `631e0a7`); the BL/call-entry continuation ABI is `631e0a7`;
  return polling moved to the fault-backed L1 safepoint in `940dcfe`; call-aware indirect L1 is
  provided by `631e0a7` and its hot boundary is tightened by `0783986`; the remaining
  `0x4026ca` narrow compare/carry chain is reduced by `736d04d`. Further work should start from a
  new live FEX join rather than extending this list.
- `661a995` follows that live join and closes the remaining flags cost on the largest block. The
  same-input FEX refresh still reports `0x4024c0` at 124 host / 86 guest instructions; SwiftVM's
  `0x402580` was 23 / 5 and the largest projected weighted gap at about 10.08M. Targets already
  published a generation-safe `call_pending_flags_host_pc`, but indirect calls cached only the
  canonical call entry and therefore paid a three-instruction full NZCV merge before every hit.
  AddressSpace now owns a separately invalidated pending-call L1 table. A proved full-NZCV call
  checks that table without publishing flags; a miss performs the merge in the grouped cold path
  and returns through the deferred-location dispatcher publisher. Signal redirects both call tables
  to the inaccessible interrupt mapping, and SMC clears both canonical and pending entries. Cold
  merge scratch is selected explicitly away from the dynamic target; this is required because a
  shared scratch can otherwise replace the target with the host NZCV value before miss recovery.
  Against `0783986`, the 4.2-second CoreMark 2k comparison covers 99.999863% of retained host weight,
  all top-30 PCs, and moves `364,386,782 -> 363,110,076` (`-1,276,706`, `-0.350371%`) with no
  common growth. `0x402580` shrinks `23 -> 21`; CoreMark 20k returns `crcfinal=0x382f` on Mac and
  Orb. The bounded Orb smallpt run completes in 3.861 seconds with zero spills and SHA-256
  `542db87b61af7a5cfff84083696082819f8608dc9c3ffaeda04b1db5b3210635`. Mac/Orb runtime interrupt,
  pending-table SMC, direct-link without stress, function and guarded-return focuses pass. No check,
  diagnostic path, environment switch or stress run remains. Applying the measured post-refresh
  reductions to the last formal FEX denominator estimates the remaining CoreMark gap at about 3.5%;
  this remains an estimate until the next full guest-instruction denominator refresh.
- `a283c6e` removes hot `current_loc` publication from continuation returns. `PopRSB` terminals now
  require the same grouped cold publisher already used by call returns, preserving miss, signal and
  dispatcher ordering without adding a second mechanism. Against `661a995`, the 5.35-second
  CoreMark 2k comparison keeps all 3,631 PCs / 4,838 versions, 100% host and entry coverage, and
  moves `363,110,333 -> 361,322,158` (`-1,788,175`, `-0.492461%`) with no common growth.
  `0x403383` shrinks `8 -> 7`, and `0x402363` shrinks `21 -> 20`; dynamic host work moves
  `363,110,103 -> 361,321,928`. CoreMark 20k returns `crcfinal=0x382f` on Mac and Orb. Mac/Orb
  direct-link without stress pass 1,281/789 assertions, function passes 252/252, guarded return
  passes 5/5, and the bounded smallpt screen retains zero spills and SHA-256
  `542db87b61af7a5cfff84083696082819f8608dc9c3ffaeda04b1db5b3210635`. No check, diagnostic path,
  environment switch or stress run remains. Applying this reduction to the last formal FEX
  denominator estimates the remaining CoreMark gap at about 3.0%; refresh both engines before
  treating it as a formal ratio.
- `48014ae` replaces cycle-cover `LDAR + CBNZ` checks with a one-instruction fault-backed poll.
  Each Runtime owns a page immediately before its aligned State payload; generated cycle edges load
  the page through a fixed negative State offset, while Signal and SMC request publication protect
  it. Existing fault metadata resumes at the corresponding cold exit stub, so flags, spills,
  `current_loc` and the SMC generation are committed by the same path as before. Clearing the last
  request restores read access. Continuation entry is now also rejected unless the Runtime has an
  initialized ReturnStackBuffer, closing the latent x25 dependency exposed by the new State
  placement. An exact same-build `a283c6e` / candidate CoreMark 2k A/B completes in 3.764/4.674
  seconds and covers 99.997693% of host weight and 99.997997% of entries. Common weighted work moves
  `313,535,840 -> 298,205,802` (`-15,330,038`, `-4.889405%`) with no growth; every changed hot PC
  loses one instruction, including `0x4034b0`, `0x4033bb`, `0x403552`, `0x403630` and `0x403688`.
  Mac/Orb direct-link without stress pass 817/771 assertions, SMC focuses pass 446/401, function
  passes 252/252 and guarded return passes 5/5. CoreMark 20k returns `crcfinal=0x382f` on both, while
  bounded Orb smallpt retains zero spills and SHA-256
  `542db87b61af7a5cfff84083696082819f8608dc9c3ffaeda04b1db5b3210635`. No stress run, check,
  diagnostic path or environment switch remains. This reduction is larger than the previous 3.0%
  estimated gap, so refresh the live FEX denominator before claiming parity or a lead.
- The requested five-item tranche is complete. Static-call flags bypass is provided by `aa4ed1b`;
  the generation-safe `BL` continuation ABI by `631e0a7`; return and cycle safepoints by `940dcfe`
  and `48014ae`; call-aware indirect L1 and its boundary reductions by `631e0a7`, `0783986` and
  `661a995`; and the remaining narrow compare/carry reduction by `736d04d`. A fresh same-input
  CoreMark 2k join against the live `f2e35f3` FEX measurement build covers 99.999931% of entries
  and 99.999887% of SwiftVM host weight. SwiftVM measures `1.789534` host instructions per guest
  instruction versus FEX `1.860078`, or `0.962075x`, a 3.7925% lead on this workload. Both use the
  same guest ELF and arguments; FEX returns `crcfinal=0x4983` with `disableavx`, multiblock enabled,
  ABI-local flags enabled and object-code caching disabled. This closes the five-item CoreMark
  target but is not evidence of parity across other workloads; the next tranche should begin with
  fresh bounded joins for smallpt and at least one branch-heavy workload.
- `5652b11` closes two target-validity holes exposed by the first bounded c-ray refresh. An empty
  continuation frame could match a zero guest return target and branch through a zero host
  continuation; an indirect-L1 key match could likewise select a cleared zero value. Continuation
  hits now require a nonzero host continuation, and the L1 condition combines key and value
  validity before selecting either the cached target or miss path. Temporary RSB-empty and
  zero-key-table prototypes were removed because neither covered both failure modes. The repaired
  c-ray `-j 1 -s 1 -d 4x3` shape completes in 10.5 seconds with 8,259 PCs; its live join covers
  97.286718% of entries and 96.183129% of SwiftVM host weight, measuring SwiftVM `2.695852` versus
  FEX `3.382138`, or `0.797085x`. The necessary checks cost CoreMark 2k
  `298,224,015 -> 300,772,945` (`+2,548,930`, `+0.854703%`) at 100% PC/entry coverage. The refreshed
  live ratio remains ahead: SwiftVM `1.804829` versus FEX `1.860078`, or `0.970297x` and a 2.9703%
  lead. Mac/Orb focused return, L1 and call-link tests pass 47/47 assertions, CoreMark 20k returns
  `crcfinal=0x382f`, and bounded smallpt retains zero spills and SHA-256
  `542db87b61af7a5cfff84083696082819f8608dc9c3ffaeda04b1db5b3210635`. No stress run, check,
  diagnostic path or environment switch remains.
- `880831e` closes two correctness failures exposed while admitting more FEX comparison workloads.
  FLAGS_REGS previously discarded a pending parity token and uncommitted NZCV whenever the next
  producer wrote any flag. The producer boundary now publishes only state that the new producer
  does not overwrite, while SSE4.2 string comparison clears its architecturally constant PF/AF
  directly instead of synthesizing two dead ALU flag producers. The SSE4.2 Rosetta/SDM matrix moves
  from 4,592 JIT/interpreter divergences to zero and passes 16,255 assertions; the alias/REX matrix
  passes 27. Separately, a continuation miss reached the cold indirect-L1 lookup with its guest
  target in `x8`; scratch allocation then emitted `ldp x8, x9; cmp x8, x8`, replacing the real
  stack target `0x4d94ae` with the empty cache key and dispatching to RIP zero. Indirect-L1,
  continuation and indirect-call forwarding now reserve their target before leasing scratch, and a
  focused cold-lookup check covers the register non-alias invariant. SQLite no longer rejects
  `--size` or falls through RIP zero, 7zip advances from RIP zero to its independent gconv cwd
  assertion, and OpenSSL reaches its SHA loop; SQLite's empty `PRAGMA threads=` and OpenSSL ignoring
  `-seconds 1` remain separate argument/value-semantics blockers, so no new cross-workload FEX ratio
  is claimed. A 10.7-11.2 second CoreMark 2k A/B keeps all 3,506 PCs / 5,236 versions, 100% coverage
  and `crcfinal=0x4983`; weighted host work changes `300,773,011 -> 300,773,475`
  (`+464`, `+0.000154%`). Mac and Orb flags, indirect-L1 and continuation focuses pass 119, 19 and
  2 assertions respectively. No stress run, full suite, diagnostic path or environment switch
  remains.
- `ac3c3de` closes the value and region-cycle failures uncovered by the bounded OpenSSL refresh.
  Full-width aliases of a value already published in a pinned guest GPR now resolve directly to
  that fixed home in scalar Add, so glibc `strtol` computes `acc * 10 + digit` instead of reading an
  uncomputed SSA allocation. The alias matcher also rejects non-`BitExtract` consumers before
  querying their return width. Backedge labels now cover both hot pending-flags cuts and cold
  dead-successor cuts, cold-path poll faults are resolved before label release, and direct-cycle
  stubs are emitted after all cold edges have declared their targets. OpenSSL now honors
  `-seconds 1` and completes its SHA run without a translation assertion. Mac and Orb pinned,
  fixed-home and region focuses pass 93, 40 and 264 assertions; CoreMark 2k retains
  `crcfinal=0x4983`. A same-input static setup region measures 634 SwiftVM host instructions versus
  597 in FEX, a 6.20% local gap. The SHA body is not statically comparable because FEX compiles a
  2,604-instruction multiblock from `0x8b9340`, while SwiftVM compiles only the two reached regions.
  A six-second counter run was stopped without output, and no profiler path or temporary check is
  retained. OpenSSL still reports `infk` because its elapsed-time denominator is zero; do not use
  that throughput as code-generation evidence.
- `89d4d94` closes two host-continuation invalidation holes without adding hit-path work. An empty
  continuation slot previously used post-index `LDP` before discovering that no host continuation
  existed, advancing x25 on every mixed-mode return miss. The empty arm now restores x25 before
  entering the existing grouped miss publisher; nonzero mismatches retain the established consume
  behavior. `ClearInterrupt` also resets the guarded continuation stack before guest signal-handler
  re-entry, so host PCs from an interrupted active epoch cannot survive asynchronous control-flow
  replacement. Mac and Orb continuation and interrupt focuses pass 3 and 8 assertions; Orb
  direct-link without stress passes 771 assertions. CoreMark 2k retains `crcfinal=0x4983`, and the
  one-second OpenSSL command again exits normally. A prototype that retained the static call target
  through `CallReturn` proved that real call sites currently miss the continuation ABI and reduced
  the `0x5567f0` setup range sharply, but it was fully removed: mixed function/non-function return
  units first exposed the empty-pop defect, and after that repair OpenSSL's alarm handler wrote
  `speed.c::run` from 1 to 0 while the interrupted SHA batch still failed to finish within the
  seven-second cap. Do not reactivate static call continuation until async resume bounds the
  interrupted batch without relying on the hot `current_loc` store.
- `70049f8` restores the default pinned/UniformElim SQLite path and closes the underlying
  caller-saved narrow-publication defect. In `sqlite3_str_vappendf` at guest `0x4250fc`, the old
  host sequence loaded the format kind with `ldrb w2` but then executed `uxtb w9, w9; cmp w9,
  #16`, so `%d` was classified from an uncomputed SSA allocation and `PRAGMA threads=%d`
  became `PRAGMA threads=`. The pinned-copy proof now admits unsigned narrow loads whose
  `SetHostGPR` publication was already coalesced, and narrow flag/compare lowering retains and
  resolves the proved fixed-home alias. The repaired sequence is `ldrb w2; uxtb w9, w2; cmp w9,
  #16`; full default SQLite `--threads 1 --size 1 --testset main` exits successfully and reports
  `TOTAL 0.973s` in a 1.58-second wall run. Mac and Orb pinned focuses pass 93 assertions each,
  CoreMark 2k retains `crcfinal=0x4983`, and the one-second OpenSSL SHA command exits normally.
  A same-input wall sample measured SwiftVM 1.955 seconds versus FEX 1.128 seconds, but this is not
  a code-generation ratio. The bounded SQL-only RE=0 static join covered only 19.76% of entries
  and 13.83% of SwiftVM host weight because FEX emitted multiblock roots while SwiftVM emitted
  per-block roots, so its apparent weighted ratio is rejected. No diagnostic check, temporary
  source path, environment switch, or benchmark stress run remains.
- `a298238` removes the remaining profile-interface hop from the pending-flags indirect-call hit
  boundary. The pending call-L1 base now lives beside the canonical indirect-call bases in `State`,
  so `0x402580` loads it directly from x28 instead of loading `RuntimeProfileInterface` first.
  Against the post-SQLite CoreMark 2k shape, all 3,695 PCs and versions match at 100% host and entry
  coverage; weighted host work moves `305,945,562 -> 305,307,232` (`-638,330`, `-0.208642%`).
  `0x402580` contributes `-638,328` and shrinks `16 -> 15`; two cold one-entry blocks contribute the
  remaining two instructions. The final tracked-source resync reproduces `305,307,232` exactly with
  `crcfinal=0x4983`. Mac and Orb interrupt and indirect-call-L1 focuses pass 8 and 29 assertions.
  The bounded default SQLite run exits in 1.61 seconds with `TOTAL 0.983s`, and the one-second
  OpenSSL SHA command exits normally. Replacing the existing flags-bypass branch with NOPs was
  rejected without implementation: the three NOPs would still execute, so removing that remaining
  branch requires a different code layout and must not introduce per-site cold growth. No stress
  run, check, diagnostic path or environment switch remains.
- `3e58efb` removes the linked static-forward branch without turning the skipped merge into executed
  NOPs. Continuation-mode static forwards emit one `BL` to a unit-shared cold publisher keyed by
  scratch/token registers; the publisher performs the full NZCV merge, parity-byte publication and
  `RET`. A generation-compatible link atomically replaces that `BL` with the parity `BFXIL`, while
  incompatible publication and SMC restore the finalized internal `BL`. Link metadata now accepts
  either the existing skip branch or the parity instruction, and disk-cache format 11 serializes
  both the unlinked and linked words. The production test covers compatible linking, invalidation,
  incompatible recompilation and real cold-publisher execution. An early prototype captured the
  forward `BL` before VIXL label finalization and restored `BL .`; the final path refreshes this
  instruction after `FinalizeCode`, and no self-loop fallback remains.
  The exact CoreMark 2k screen keeps all 3,695 PCs/versions, 100% host and entry coverage and
  `crcfinal=0x4983`; hot-shape weight moves `305,307,232 -> 299,985,184` (`-5,322,048`,
  `-1.743178%`) with no growing PC. `0x403552`, `0x402830` and `0x403550` each remove the three
  inline merge instructions and contribute `-3,696,000`, `-1,098,000` and `-528,000`. This static
  number is not a runtime instruction claim: after the old runtime skip patch, the strict linked
  path improves by one executed instruction per compatible traversal. Across 291 common emitted
  units, shared cold publishers add 324 total bytes (`282,848 -> 283,172`, `+0.1146%`). Mac and
  Orb flags-link, serializer, indirect-L1, call-continuation, interrupt and guarded-return focuses
  pass 168 assertions each. Bounded smallpt retains oracle
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`; default SQLite exits in
  1.59 seconds with `TOTAL 0.974s`, and the one-second OpenSSL SHA command exits normally. No stress
  run, check, diagnostic path or environment switch remains.
- `1dff969` reduces the validated return-continuation hit from five instructions to four:
  `LDP; CMP; B.ne miss; BLR continuation`. Ordinary empty frames and guest-target mismatches branch
  to the existing grouped cold publisher, which reloads x25 from the Runtime's immutable
  `State::rsb_empty` before dispatching the actual guest target. The only equality corner that can
  still reach a zero host continuation is an empty frame with actual guest target zero. `BLR 0`
  leaves its source PC in x30; the signal handler accepts `LR-4` only when it resolves to a precise
  `ContinuationMiss` fault record, resets x25 to Empty and resumes the same cold publisher. Other
  null branches and guest address-zero faults retain their existing handling. Fault records carry
  this recovery kind through disk-cache format 12.
  The exact CoreMark 2k comparison keeps all 3,695 PCs/versions, 100% coverage and
  `crcfinal=0x4983`; hot-shape weight moves `299,985,184 -> 292,339,525` (`-7,645,659`,
  `-2.548679%`) with no growth. `0x403383` shrinks `9 -> 6` and contributes `-4,656,000`; every
  changed return block loses three static instructions. The strict successful-hit saving is one
  executed instruction per traversal. Across 291 common emitted units, total code size also falls
  `283,172 -> 279,760` (`-3,412` bytes) with no growing unit. A rejected all-mismatch-fault variant
  kept the same hot shape but moved SQLite to `TOTAL 1.825/1.833s`; it is fully removed. The final
  branch-to-cold design restores bounded SQLite to `TOTAL 1.020/1.000s` in 1.64/1.62-second wall
  runs. Mac/Orb function, direct-link without stress, target-zero/mismatch, guarded-return,
  interrupt and serializer focuses pass 1,174/1,111 assertions. Bounded smallpt retains oracle
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`, and the one-second
  OpenSSL SHA command exits normally. No stress run, check, diagnostic path or environment switch
  remains.
- `e0f9d58` removes the nonzero host-entry check from every continuation indirect-call hit. The hot
  check is now `LDR base; BFI address; LDP key/entry; CMP key; B.ne miss; BLR entry`, six
  instructions. Ordinary key mismatches still branch to the existing grouped cold path. Only a
  matching entry whose value was atomically invalidated to zero reaches `BLR 0`; x30 identifies the
  exact source instruction, an `IndirectCallMiss` fault record resumes that same cold path, and x25
  is left unchanged because no continuation frame was pushed. Interrupt-table faults retain their
  existing lookup recovery. Disk-cache format 13 serializes and validates the new recovery kind.
  The exact CoreMark 2k join keeps all 3,695 PCs/versions, 100% host and entry coverage, all top-20
  PCs and `crcfinal=0x4983`; weighted host work moves `292,339,525 -> 291,701,194`
  (`-638,331`, `-0.218353%`) with no growth. `0x402580` shrinks `15 -> 14` and contributes
  `-638,328`. Across 291 common emitted units, code size moves `279,760 -> 279,652` (`-108` bytes),
  with 18 shrinking and none growing. A same-build Orb SQLite A/B is neutral within run noise:
  baseline wall `1.836/1.623/1.589s`, candidate `1.704/1.678/1.620s`; tracing the candidate records
  zero SIGSEGV/SIGBUS, confirming ordinary misses do not use the new recovery. Mac/Orb function,
  direct-link without stress, guarded-return and interrupt focuses pass 252/252, 1,099/807, 5/5
  and 8/8 assertions; the new invalidated-entry recovery passes 11/11 and the v13 serializer focus
  passes 41/41. Bounded smallpt retains oracle
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`, and the one-second
  OpenSSL SHA command exits normally. No stress run, retained check, diagnostic path or environment
  switch remains.
- The post-`e0f9d58` live FEX refresh uses the same measurement binary and smallest-containing-range
  join as the prior report. Replaying the retained pre-refresh SwiftVM profile reproduces the old
  result to rounding: SwiftVM `2.004543`, FEX `1.860013`, `1.077704x`. The current CoreMark 2k
  profile measures SwiftVM `1.749840` versus FEX `1.860484`, or `0.940530x`, at 99.999986% guest
  and 99.999973% SwiftVM-host coverage. CoreMark is therefore no longer the workload-level gap to
  optimize, even though isolated blocks still exceed FEX's multiblock-density estimate. A bounded
  smallpt join also favors SwiftVM on the 98.45% comparable guest path, but FEX produces a different
  PPM, so that result is only an exclusion signal and not a parity claim. SQLite remains the next
  branch-heavy audit target. Its full entry-instrumented run did not finish inside either the six-
  or eight-second cap and was terminated; SIGINT did not produce a partial profile. Do not extend
  those runs or add a profiler exit mechanism. Use a bounded representative testset or inspect the
  remaining multiblock/dispatch formation directly. No new profiler, check, environment switch or
  source path was retained.
- `30c85c7` removes the preserve-all helper ABI from BSF/BSR. The frontend now emits U64
  count-zero IR, AArch64 lowers BSF to `RBIT + CLZ` and BSR to `CLZ + EOR #63`, and the
  interpreter provides the same zero-count semantics. The old `Bsf64`/`Bsr64` helpers and their
  fallback path are deleted. In SQLite `__memcmp_sse2` this removes the repeated complete register
  captures: STP/LDP fall from 122/137 to 2/17, while the unit shrinks `1,349 -> 1,017` host
  instructions. The bounded `main/10` static set retains all 1,995 units and moves
  `497,244 -> 495,370` (`-1,874`, `-0.3769%`); 16 units shrink and one grows. Two interleaved full
  SQLite pairs are consistently but modestly faster: internal totals `1.182/1.177s ->
  1.173/1.169s`, with wall `1.452/1.442s -> 1.437/1.434s`. The exact CoreMark 2k screen retains all
  3,695 PCs/versions and 100% coverage with no growth, moving `291,701,260 -> 291,700,688`
  (`-572`). Bounded smallpt keeps SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac/Orb directed bit-scan
  cases pass; fixed-seed 424242 bit fuzz records zero BSF mismatches and the same unrelated ROL
  divergence family. No stress run, full suite, check, diagnostic path or new environment switch
  remains.
- `4c9c669` removes both preserve-all helper calls from every 8/16/32-bit DIV/IDIV. The combined
  dividend already fits U64 at those widths, so the frontend now emits native unsigned division or
  the dedicated U64-typed signed division IR, then derives the remainder arithmetically. The
  established zero-divisor result remains quotient/remainder zero. Only the real 128/64 case keeps
  `DivQU64`/`DivRU64`/`DivQS64`/`DivRS64`. The bounded SQLite `main/10` set keeps all 1,995 units
  and moves `495,370 -> 488,179` (`-7,191`, `-1.451642%`), with 46 shrinking units and no growth.
  Two interleaved full SQLite pairs move internal totals `1.185/1.286s -> 1.181/1.215s` and wall
  `1.484/1.562s -> 1.456/1.486s`. CoreMark 2k keeps all 3,695 PCs/versions at 100% coverage with no
  growth and moves `291,700,693 -> 291,700,603` (`-90`). Fixed-seed 101 and 424242 DIV/IDIV fuzz
  pass on Mac and Orb. Bounded smallpt keeps SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, full
  suite, check, diagnostic path or new environment switch remains.
- `26c5f40` removes repeated direct-cycle cold exit tails from large function units. Each fault
  recovery label still acquires the exit request, publishes its block-specific flags and writes
  its exact target; functions with at least five conservative candidates branch afterward to one
  shared halt-reason/host-return tail. Smaller functions keep the original inline tail so the
  screen has no growing unit. The bounded SQLite `main/10` set keeps all 1,995 units and moves
  `488,179 -> 468,505` (`-19,674`, `-4.030079%`), with 306 shrinking units and no growth;
  `0x4a5518` and `0x4a5470` shrink by 134 and 119 instructions. An exact same-build, order-reversed
  full SQLite pair is neutral-to-positive: internal totals `1.288/1.281s -> 1.272/1.277s`, wall
  `1.594/1.565s -> 1.559/1.564s`. CoreMark 2k is byte-identical across all 3,695 PCs/versions at
  100% coverage. Mac/Orb direct-cycle, pending-interrupt and disabled-latch focuses pass 65
  assertions each. Bounded smallpt keeps SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, full
  suite, check, diagnostic path or new environment switch remains.
- A CPUID prototype that retained all four output registers in SSA until the final publication was
  fully removed. It lengthened four simultaneous Select chains and increased register pressure:
  bounded SQLite moved `468,505 -> 469,934` (`+1,429`), all five changed CPUID units grew, and
  `get_common_cache_info` grew `1,594 -> 1,821`. A future CPUID reduction must use compact
  leaf-directed control flow or a dedicated lowering rather than extending all output lifetimes.
- CPUID output markers and a CPUID-scoped pinned-register constant tracker were also fully removed.
  They kept the existing branchless table live and added marker pressure without folding its final
  outputs: the three bounded variants moved SQLite by `+135`, `+179` and `+164`, with the same five
  CPUID units growing and none shrinking. Do not retry a value-DAG wrapper around the current table.
- `8c4f055` compacts the byte `VecMovMask` lowering used by PMOVMSKB. The input is first reduced to
  one sign bit per byte; three `USRA`/`XTN` levels then combine adjacent 1-, 2- and 4-bit groups
  without a constant vector or scalar bridge. Each occurrence falls from 11 to 8 AArch64
  instructions. Bounded SQLite `main/10` keeps all 1,995 units and moves `468,505 -> 468,118`
  (`-387`, `-0.082603%`), with 22 shrinking units and no growth. `__strrchr_sse2` moves
  `1,041 -> 957`, `__memcmp_sse2` moves `1,017 -> 966`, and the exact 735-unit FEX join moves from
  `1.0854x` to `1.0828x` (`157,757 / 145,699`). The order-reversed full SQLite wall samples are
  neutral-to-positive, and bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The directed SSE batch
  passes 129 assertions on Mac and Orb; a fixed 20-iteration SSE2 seed retains the baseline's 11
  unrelated flag divergences. No stress run, check, diagnostic path or environment switch remains.
- `9ac80fd` moves that hierarchy into one vector-lowering helper and uses it to collect byte
  IntRes1 masks in inline PCMPISTRI/M. This removes the synthesized 128-bit weight constant, two
  horizontal reductions and the scalar high-half merge from every byte-form `Sse42Str`. Against
  `8c4f055`, bounded SQLite `main/10` keeps all 1,995 units and moves `468,118 -> 467,641`
  (`-477`, `-0.101897%`), with 16 shrinking units and no growth. `__strcspn_sse42` moves
  `806 -> 752`; the exact 735-unit FEX join moves to `157,334 / 145,699`, or `1.0799x`. The helper
  fallback was measured separately and rejected (`+2,907`, all 16 affected units grew), so the
  inline path remains canonical. Mac and Orb pass the 16,255-assertion Rosetta/SDM differential,
  the scratch-contract and PMOVMSKB directed gates; bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, check,
  diagnostic path or environment switch remains.
- `4994d73` replaces the separate quotient and remainder helpers for 128/64 DIV/IDIV with
  one `Div128` operation whose AArch64 helper returns both ABI results. The shared arithmetic moved
  out of the x86 decoder, and the interpreter consumes the same implementation. A remainder pseudo
  gives register allocation an independent second result without extending the IR to a general
  tuple type. Function mode exposed one backend boundary: a spilled pseudo result requested while
  emitting its producer was treated as an old spill-slot read. `RForWrite` now allocates that
  destination and schedules its normal writeback. Default function mode and `SVM_FUNC_BASE=0` both
  complete bounded SQLite `main/10`; fixed-seed 101 and 424242 DIV/IDIV fuzz pass on Mac and Orb.
  Against `9ac80fd`, the 1,995-unit static set moves `467,641 -> 465,921` (`-1,720`,
  `-0.367804%`), with 26 shrinking units and one growing unit. `sqlite3VdbeExec` at `0x4a5518`
  shrinks `1,110 -> 980`, and `get_common_cache_info` at `0x4d8ce0` shrinks `1,594 -> 1,498`.
  Bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, retained
  check, diagnostic source path or new environment switch remains.
- `3f38207` replaces CPUID's four parallel leaf-selection chains with one x86-specific semantic
  helper returning two packed 64-bit results. The helper receives the dynamic leaf/subleaf and a
  compile-time feature mask, so code-cache-visible feature gating remains identical while the hot
  IR no longer keeps every candidate output live. A shared pair-result ABI now serves both CPUID
  and 128-bit division; the AArch64 backend owns the common preserve-all call lowering, and the
  interpreter invokes the same CPUID model. Against `4994d73`, bounded SQLite `main/10` keeps all
  1,995 units and moves `465,921 -> 464,464` (`-1,457`, `-0.312714%`), with all five CPUID units
  shrinking and none growing. `get_common_cache_info` at `0x4d8ce0` moves `1,498 -> 1,226`; the two
  larger CPUID roots move `2,404 -> 1,703` and `999 -> 621`. Mac and Orb pass the fixed-seed CPUID
  differential, XSAVE's three configuration cases, FSGSBASE/ADX gating, PKRU non-advertisement and
  the paired-division regression. SQLite exits normally, CoreMark 20k retains `crcfinal=0x382f`,
  and bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, fallback,
  retained check, diagnostic source path or new environment switch remains.
- `58e8d58` detects FEAT_LSE through the existing host-feature bitmap and lowers aligned scalar
  CMPXCHG, XCHG and XADD to CASAL, SWPAL and LDADDAL. The capability is already part of ConfigHash,
  so cached code cannot cross into a non-LSE host shape. Misaligned x86 atomics keep the required
  serialized basic-access path; only the aligned exclusive retry loops are removed. Against
  `3f38207`, bounded SQLite `main/10` keeps all 1,995 units and moves `464,464 -> 464,305`
  (`-159`, `-0.034233%`), with 25 shrinking units and no growth. `__run_exit_handlers` moves
  `1,010 -> 986`; its ten aligned atomic sites become four CASAL and six SWPAL instructions, while
  the static unaligned lock paths remain. Fixed-seed 101 and 424242 bit-operation fuzz on Mac and
  Orb report zero CMPXCHG mismatches; only the established unrelated rotate/bit families remain.
  Bounded SQLite exits normally. No stress run, check, diagnostic source path or new environment
  switch remains.
- `3c5a67a` adds an explicit general-register-only helper contract for AArch64 Clang and GCC and
  applies it to the CPUID pair-result helper. CPUID now receives the XSAVE/YMM configuration in its
  compile-time feature mask, so the helper is a closed integer leaf; Orb disassembly has 113
  instructions, no SIMD operand and no call. The backend therefore omits the pinned SIMD capture
  only when the compiler enforces that contract. Against `58e8d58`, bounded SQLite `main/10` keeps
  all 1,995 units and moves `464,305 -> 463,873` (`-432`, `-0.093042%`), with all five CPUID units
  shrinking and none growing. The larger CPUID roots move `1,703 -> 1,495` and `621 -> 509`, while
  `get_common_cache_info` moves `1,226 -> 1,146`.
- The same commit moves the process-wide unaligned-atomic lock address into runtime state. Scalar
  CMPXCHG, XCHG, XADD and generic locked RMW cold paths load it once and retain it across acquire
  and release; 128-bit CAS reloads it only where its observed pair consumes both reserved atomic
  scratch registers. Against the preceding helper shape, SQLite moves `463,873 -> 463,553`
  (`-320`, `-0.068984%`), with 25 shrinking units and no growth; `__run_exit_handlers` moves
  `986 -> 936`. Mac and Orb fixed-seed 101/424242 bit fuzz report zero CMPXCHG mismatches, and both
  hosts pass the CPUID, XSAVE, FSGSBASE/ADX and PKRU focuses. From the pre-division `9ac80fd` shape,
  the five completed mechanisms move SQLite `467,641 -> 463,553` (`-4,088`, `-0.874175%`). Bounded
  SQLite exits normally, CoreMark 20k retains `crcfinal=0x382f`, and smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, retained
  check, diagnostic source path or new environment switch remains.
- `b3c30f8` moves cycle-poll resume-location publication from every generated cold recovery stub
  into the existing fault metadata path. Each poll fault already has an exact serialized
  `guest_start`; resolution now changes that field to the architectural resume location, and the
  runtime signal handler writes `state->current_loc` before entering the flags/halt recovery code.
  Backedge stubs retain the source block and directed-cycle stubs retain their edge target, while
  the repeated two-instruction guest-PC materialization and state store disappear. Against
  `3c5a67a`, bounded SQLite `main/10` keeps all 1,995 units and moves `463,553 -> 436,665`
  (`-26,888`, `-5.800415%`), with 1,421 shrinking units and no growth. `0x4a5518` moves
  `980 -> 896`, `0x4a5470` moves `656 -> 581`, `0x4e8720` moves `837 -> 783`, and
  `get_common_cache_info` moves `1,146 -> 1,080`. From the pre-division `9ac80fd` shape, the
  cumulative movement is `467,641 -> 436,665` (`-30,976`, `-6.623885%`). Mac and Orb pass the
  non-stress signal group, local-cycle SMC, directed-cycle interrupt, disabled-latch, production
  SMC ring, guarded-return and disk-cache fault-site round-trip focuses. Bounded SQLite exits
  normally, CoreMark 20k retains `crcfinal=0x382f`, and smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, retained
  check, diagnostic source path or new environment switch remains.
- `c7107cd` lowers 32/64-bit x86 ROL/ROR value paths through the existing native `RorImm` and
  `RorValue` IR instead of synthesizing two shifts and an OR. Immediate left rotates use the
  complementary right-rotate count, dynamic left rotates use the low-width negated count, and
  zero counts retain the original value and flag-preservation path. The 8/16-bit lowering and all
  CF/OF construction remain unchanged. Against `b3c30f8`, bounded SQLite `main/10` keeps all 1,995
  units and moves `436,665 -> 435,777` (`-888`, `-0.203360%`), with eight shrinking units and no
  growth. `sqlite3_randomness` at `0x423620` moves `2,300 -> 1,765`, and its alternate root at
  `0x4236c0` moves `637 -> 367`. From `9ac80fd`, the cumulative movement is
  `467,641 -> 435,777` (`-31,864`, `-6.813774%`). Mac and Orb fixed-seed 101/424242 bit fuzz retain
  their exact established rotate/BT mismatch counts with zero CMPXCHG mismatches; the narrow
  rotate directed test passes. Bounded SQLite exits normally, CoreMark 20k retains
  `crcfinal=0x382f`, and smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, retained
  check, diagnostic source path or new environment switch remains.
- `5df9b59` splits rotate flag updates by the decoder's already-known count class. Dynamic counts
  retain the zero-count branch and conditional carry-polarity merge, statically zero counts retain
  both incoming flags and polarity without emitting an update, and statically nonzero counts write
  CF/OF directly without the synthesized `CMP + CSET + CBZ`. Against `c7107cd`, bounded SQLite
  `main/10` keeps all 1,995 units and moves `435,777 -> 435,491` (`-286`, `-0.065630%`), with the
  same eight rotate units shrinking and no growth. `sqlite3_randomness` moves `1,765 -> 1,659` and
  its alternate root moves `367 -> 263`; cumulative movement from `9ac80fd` is
  `467,641 -> 435,491` (`-32,150`, `-6.874932%`). The Mac/Orb fixed seeds retain their exact
  established rotate/BT mismatch counts with zero CMPXCHG mismatches; the zero-count repro and
  narrow rotate test pass. Bounded SQLite exits normally, CoreMark 20k retains `crcfinal=0x382f`,
  and smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, retained
  check, diagnostic source path or new environment switch remains.
- `d9f2922` gives direct scalar state accesses, shifts, extensions and logical operations their
  measured per-instruction GPR scratch prices. After the ordinary Linux allocation proves that a
  unit spills and reserves x18 for reloads, the verified allocator tries one lower dynamic scratch
  reserve before its existing ladder. Non-spilling units retain the original allocation path, and
  any candidate that cannot satisfy the emitter plus reload contract falls back immediately. An
  exact HEAD/candidate SQLite `main/10` in-memory A/B keeps the same 2,065 PCs and versions, 100%
  host and entry coverage, 28 shrinking units and no growth. Static host instructions move
  `446,280 -> 445,666` (`-614`, `-0.137582%`); `sqlite3_randomness` moves `1,659 -> 1,450` and its
  alternate root moves `263 -> 209`. Timing-adjusted SQLite output is byte-identical. The local
  and Orb spill-eviction, saturated scratch-pool and hidden-scratch contract cases pass, while
  bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. A reserve of one produces
  the same SQLite shape as reserve two, so the less aggressive reserve remains canonical. No stress
  run, retained check, diagnostic source path or new environment switch remains.
- `2d74b5b` replaces SSE4.2 string comparison's generic packed-flag expansion with a dedicated IR
  publication. The shared result layout now places SF/ZF/CF/OF in a reversible nibble, the flags
  pass deletes dead publications and narrows surviving ones to the live subset, and the AArch64
  backend writes that subset directly into the flags carrier. `Sse42Str` omits flag calculation
  entirely when the publication is dead. The exact SQLite `main/10` in-memory A/B keeps the same
  2,065 PCs and versions with 100% host and entry coverage, 14 shrinking units and no growth.
  Static host instructions move `445,666 -> 444,836` (`-830`, `-0.186238%`);
  `__strcspn_sse42` moves `737 -> 645`, and the remaining 13 reductions cover the same glibc
  SSE4.2 string family. Timing-adjusted SQLite output is byte-identical. The 16,255-assertion
  Rosetta/SDM differential, aliasing, memory-boundary, scratch-contract and flag-elimination cases
  pass locally and on Orb. Bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, retained
  check, diagnostic source path or new environment switch remains.
- `f7a6461` applies the existing compiler-enforced general-register-only helper contract to paired
  128/64 division. Both signed and unsigned wrappers are integer-only; Orb's GCC 13 wrappers contain
  no SIMD instruction, and the resolved `__divti3`, `__modti3`, `__udivti3` and `__umodti3`
  implementations contain neither SIMD instructions nor nested calls. The JIT therefore omits
  resident FPR captures while retaining the existing GPR, link and paired-result preservation.
  The exact SQLite A/B keeps the same 2,065 PCs and versions with 100% coverage, 29 shrinking units
  and no growth. Static host instructions move `444,836 -> 444,116` (`-720`, `-0.161857%`);
  `sqlite3VdbeExec` at `0x4a5518` moves `896 -> 848`, and `_IO_new_file_xsputn` moves
  `783 -> 767`. Fixed-seed 101/424242 DIV/IDIV fuzz passes locally and on Orb, timing-adjusted
  SQLite output is byte-identical, and bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No stress run, retained
  check, diagnostic source path or new environment switch remains.

- Published region entries now share their counted entry when no split backedge ABI is required.
  Dispatcher, region-link slow traversal, indirect-L1 and legacy RSB paths restore NZCV before
  entering that shared address, while direct links retain their already-valid PSTATE path. This
  removes the per-block `MSR NZCV; B counted-entry` veneers. An exact same-machine, same-command
  SQLite `main/10` static-only A/B covers all
  1,999 units and moves `432,331 -> 384,068` host instructions (`-48,263`, `-11.163437%`), with
  1,414 shrinking units and 80 one-instruction growths. The largest current gaps move as follows:
  `0x4a5518 848 -> 748`, `0x4a5470 581 -> 506`, `0x449880 706 -> 647`,
  `0x4e8720 767 -> 677`, `0x4e2380 839 -> 746`, `0x506d30 645 -> 610` and
  `0x50b870 918 -> 871`. Timing-adjusted SQLite output remains byte-identical with SHA-256
  `efb4bea2cec1e324d746acc11d14f62c8c742fcd98f1a52f4b75a7e44a864a25`; bounded smallpt retains
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Local and Orb entry,
  direct-link, continuation, guarded-return and flags focuses pass 169 assertions across nine
  cases. No stress run, check, diagnostic source path or environment switch remains.

- `CQO`-formed signed `Div128` pairs now lower through native `SDIV + MSUB` when the optimized
  operand chain still proves that the high half is `ASR(low, 63)`. Basic `GetOperand` and `BitCast`
  wrappers are resolved without accepting shifted, extended or composite operands; all other
  128/64 divisions retain the integer-only paired helper. The quotient uses a scratch register
  reserved after the paired remainder output, preventing the remainder home from aliasing and
  destroying the quotient. An exact SQLite `main/10` static-only A/B keeps all 1,999 units at 100%
  coverage and moves `384,068 -> 383,923` host instructions (`-145`, `-0.037754%`), with four
  shrinking units and no growth. `0x4a5518` moves `748 -> 678`; `0x4d8ce0`, `0x42327e` and
  `0x41099c` each lose 25 instructions. Timing-adjusted SQLite output remains byte-identical with
  SHA-256 `efb4bea2cec1e324d746acc11d14f62c8c742fcd98f1a52f4b75a7e44a864a25`, and bounded smallpt
  retains `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass
  fixed seeds 101/424242 with 100 random DIV/IDIV iterations plus directed `CQO; IDIV` cases. No
  stress run, check, diagnostic source path or environment switch remains.

- Direct-cycle fault recovery now performs the acquire load of `exit_request` once in the shared
  reason tail instead of once per resume-location stub. Each stub still owns its fault metadata and
  publishes pending flags before joining the tail, so Signal versus CodeMiss selection and guest
  resume PCs are unchanged. An exact SQLite `main/10` static-only A/B keeps all 1,999 units at 100%
  coverage and moves `383,923 -> 379,915` host instructions (`-4,008`, `-1.043959%`), with 307
  shrinking units and no growth. `0x4a5518` moves `678 -> 651`, `0x4a5470` moves `506 -> 482`, and
  the largest single reduction is 29 instructions. Timing-adjusted SQLite output remains
  byte-identical with SHA-256
  `efb4bea2cec1e324d746acc11d14f62c8c742fcd98f1a52f4b75a7e44a864a25`; bounded smallpt retains
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  function-SMC cycle, pending-interrupt, disabled-latch and W81 focused paths. No stress run, check,
  diagnostic source path or environment switch remains.

- Implicit byte-form `PCMPISTRI` with immediate `0x3a` and identical physical operands now uses its
  reduced SDM semantics directly. Equal-each produces all ones, masked-negative leaves exactly the
  invalid suffix, the index is the shared first-zero length, SF/ZF/CF are `length < 16`, and OF is
  `length == 0`; one length scan therefore replaces the generic dual-length, aggregation and
  movemask pipeline. An exact SQLite `main/10` static-only A/B keeps all 1,999 units at 100%
  coverage and moves `379,915 -> 379,667` host instructions (`-248`, `-0.065278%`), with seven
  shrinking glibc SSE4.2 units and no growth. `__strcspn_sse42` moves `610 -> 548`; the other six
  roots each lose 31 instructions. Timing-adjusted SQLite output remains byte-identical with
  SHA-256 `efb4bea2cec1e324d746acc11d14f62c8c742fcd98f1a52f4b75a7e44a864a25`, and bounded smallpt
  retains `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  16,255-assertion Rosetta/SDM differential, the 512-assertion scratch contract, four memory-boundary
  assertions and 27 alias/REX assertions. No stress run, check, diagnostic source path or
  environment switch remains.

- A dedicated ARM64 `REP MOVS` loop was evaluated against the two helper captures in
  `sqlite3_randomness` at `0x423620` and fully rejected. The candidate kept precise page-fault PCs,
  copied forward and backward in architectural element order, and passed Mac/Orb fixed-seed fuzz,
  the interpreter path and the bounded smallpt oracle. Its bounded-window checks and fixed-clobber
  pressure nevertheless moved the exact 1,999-unit SQLite static shape from `379,667 -> 384,298`
  (`+4,631`, `+1.219753%`), while `0x423620` grew `1,360 -> 1,477`. Do not retry an inline loop until
  the guest window has a guaranteed guard-page ABI that removes per-site bounds code, or the loop
  can be expressed with materially fewer fixed clobbers. No implementation or check remains.

- A low-32-bit `BitExtract` can now reuse the W view of its U64 source across separated or repeated
  read-only consumers. The allocator requires the source SSA lifetime, rather than merely an
  occupied physical register, to cover the complete view lifetime and rejects every consumer whose
  output could overwrite either register. The emitter independently re-proves the same lifetime and
  alias constraints before removing the `UBFX`/`LSR #0`. An exact SQLite `main/10` static-only A/B
  keeps all 1,999 units at 100% coverage and moves `379,667 -> 379,445` host instructions (`-222`,
  `-0.058472%`), with 136 shrinking units and no growth; the largest unit loses eight instructions.
  Timing-adjusted SQLite output is byte-identical, and bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the two
  focused allocation cases with 12 assertions. No stress run, check, diagnostic source path or
  environment switch remains.

- A reverse producer-to-low32-view remap was evaluated and rejected. Keeping the producer's old
  register reserved moved `379,445 -> 379,283` but grew 21 units; releasing it exposed an unmodelled
  producer/pseudo physical-register contract and failed during early SQLite execution. Do not retry
  this direction until that ownership is represented explicitly in RA. No implementation remains.

- Region-internal cycle coverage now uses only the exact DFS backedges already computed by
  `PrepareRegionEdges`; the total-order descending cut remains only on linkable external edges,
  whose LinkManager sites are synchronously detached by SMC invalidation. Exact local backedge and
  external direct-cycle stubs now contribute to the same function-wide reason-tail sharing decision.
  An exact SQLite `main/10` static-only A/B keeps all 1,999 units at 100% coverage and moves
  `379,445 -> 350,638` host instructions (`-28,807`, `-7.591878%`), with 933 shrinking and 34
  growing units. `0x4a5470` moves `482 -> 440`, `0x428280` moves `828 -> 775`, and
  `sqlite3_randomness` at `0x423620` moves `1,358 -> 1,273`. Timing-adjusted SQLite output is
  byte-identical, and bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb each pass
  123 assertions across exact region-cycle invalidation, external direct-link rings, conditional
  rings, pending interrupts and the disabled latch. No stress run, check, diagnostic source path or
  environment switch remains.

- The function-wide cycle reason tail now shares at its exact two-stub break-even instead of waiting
  for five candidates. Against the exact-cycle baseline, SQLite keeps all 1,999 units and moves
  `350,638 -> 344,221` host instructions (`-6,417`, `-1.830093%`), with 303 shrinking units and no
  growth. `0x425378` moves `1,032 -> 919`, while `0x4a882e` moves `855 -> 766`. Timing-adjusted
  SQLite output remains byte-identical, bounded smallpt retains SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`, and Mac/Orb repeat the
  same 123 cycle/SMC assertions. No stress run, check, diagnostic source path or environment switch
  remains.

- Full-NZCV publication on a bypassable direct link now uses a two-instruction deoptimization site:
  `ADR x17, resume; B region_merge`. Compatible pending-flags targets patch the first instruction to
  skip both words; incompatible targets branch to one code-region merge trampoline and return through
  `x17`. This preserves the host continuation in `x30`, unlike a callable per-unit stub. Disk-cache
  format v14 records and adjusts the merge-branch offset, then relocates it to the current region
  trampoline on revival. Exact SQLite `main/10` static-only A/B keeps all 1,999 units and moves
  `344,221 -> 340,875` host instructions (`-3,346`, `-0.972050%`), with 1,049 shrinking units and no
  growth. `0x4a5518` moves `615 -> 599`, `0x4a882e` moves `766 -> 752`, and `0x401e94` moves
  `549 -> 535`. Timing-adjusted SQLite output remains byte-identical with SHA-256
  `9fb4d81a33c7f75c5b122f609410088c3be8a2a0bf5458d1e1164ecfc19f22f2`; bounded smallpt retains
  SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac passes 476
  assertions across production bypass execution, disk-cache serialization/revival and continuation
  preservation, plus the unchanged 123 cycle/SMC assertions. Orb passes the same cycle/SMC set and
  302 final production/cache/continuation assertions. A per-unit `BL`-return outline was rejected: it
  both grew SQLite by 1,717 instructions and overwrote `x30` before continuation-preserving direct
  links. No check, diagnostic path or environment switch from that evaluation remains.

- Full-NZCV publication in out-of-line recovery and cycle paths now uses allocation-relative merge
  sites patched to one of two code-region trampolines. The basic form carries only live PSTATE; the
  token form first materializes the parity/AF byte in `x12`. Both use `ADR x17, resume; B trampoline`
  and return through `x17`, so `x30` remains the host continuation. Units that cannot obtain a
  reachable region trampoline are re-emitted through the existing non-direct allocation path and do
  not generate these sites. Exact SQLite `main/10` static-only A/B keeps all 1,999 units and moves
  `340,875 -> 333,452` host instructions (`-7,423`, `-2.177631%`), with 1,299 shrinking units and no
  growth. `0x4032a2` moves `694 -> 654`, `0x402f73` moves `617 -> 581`, and `0x4ee9a5` moves
  `829 -> 797`. Remaining `MRS NZCV` falls from 11,694 to 5,751; full merges fall from 8,012 to
  2,069, while the 3,680 partial merges are unchanged and are now the larger publication class.
  Timing-adjusted SQLite output remains byte-identical with SHA-256
  `9fb4d81a33c7f75c5b122f609410088c3be8a2a0bf5458d1e1164ecfc19f22f2`; bounded smallpt retains
  SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac passes 616 final
  assertions across cache, production bypass, continuation, cycle/SMC and trampoline coverage; Orb
  passes the corresponding 442 assertions. A pending-flags return-continuation tag was audited but
  not emitted: without a second continuation entry it grows static code before it can remove the hot
  merge. Partial-merge follow-ups are also below the mechanism threshold: only 14 sites can use the
  existing pending direct-link ABI, while a strict no-fault/no-observer full-overwrite chain moves
  SQLite only `333,452 -> 333,347` (`-105`) across 22 units. Neither prototype remains. No census
  logging, check path, stress run or environment switch remains.

- `fb93462` moves the repeated cycle-exit reason tail into the existing code-region trampoline.
  Single-stub exits and function-shared cycle labels now branch to one region entry, which performs
  the acquire request load, selects `CodeMiss` or `Signal`, publishes the halt reason and reaches the
  existing shared return entry. A unit that cannot use a reachable region trampoline retains the
  inline tail through the existing non-direct re-emission path. The entry offset is derived from the
  fixed pending-flags trampoline layout, so `CodeRegion` and link-patch metadata do not grow. Exact
  SQLite `main/10` static-only A/B keeps all 1,999 units and moves `333,452 -> 319,814` host
  instructions (`-13,638`, `-4.089944%`), with 1,266 shrinking units, 733 unchanged units and no
  growth. `0x529e28` moves `753 -> 675`, `0x4675b0` moves `769 -> 697`, and `0x4a5518` moves
  `584 -> 536`. Timing-adjusted SQLite output remains byte-identical with SHA-256
  `9fb4d81a33c7f75c5b122f609410088c3be8a2a0bf5458d1e1164ecfc19f22f2`; bounded smallpt retains
  SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  non-stress direct-link, SMC, continuation and region groups with 941 and 875 assertions. The new
  focused region entry check covers both reason values with six assertions. No stress run, check,
  diagnostic path or environment switch remains.

- Moving static `current_loc` publication from callers into shared call entries was evaluated and
  fully removed. Even after terminal-level continuation eligibility filtering, the rebuilt short
  SQLite run halted at guest `rip=0x3e8`; the publication is part of a stricter call-entry/state
  ordering contract than the link-site metadata alone proves. Do not retry this as a simple store
  relocation without first representing that ordering in the continuation ABI.

- Function-region decoding now stops before the nearest HIR block entry already discovered by the
  worklist and links to that block instead of decoding its sequential suffix a second time. The stop
  is ignored when it falls inside the current x86 instruction, preserving legal overlapping entry
  streams. This first stage covers entries known before a block starts; entries discovered only by a
  branch at the end of that same block still require the replay stage below. A contemporaneous
  SQLite `main/10` static-only A/B matches 2,119 PCs, all top 20 roots, 99.756435% host coverage and moves
  common host instructions `333,389 -> 317,838` (`-15,551`, `-4.664521%`). The final bounded run
  retains the same candidate common total and timing-adjusted output is byte-identical. The
  smallpt `4 8 6` common set moves `45,968 -> 43,816` (`-2,152`, `-4.681518%`); its final shape is
  identical to the initial candidate capture and the PPM SHA-256 remains
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. CoreMark retains final CRC
  `0x382f`. The newly exposed narrow `Xor` block also corrected logical-op scratch accounting for
  pinned U8/U16 inputs. Mac and Orb pass the focused decoder/function tests, non-stress direct-link
  group and continuation group. No stress run, check, diagnostic path or environment switch remains.

- A decoded function block is now replayed when its terminal discovers a block entry strictly inside
  the exact guest byte span represented by its `AdvancePC` instructions. Replay removes the old CFG
  edges, terminal value uses, HIR instructions/value uses and guest-code dependencies before decoding
  only up to the new boundary. Blocks that own a call-return target keep their original decode because
  replaying only the source would invalidate that target ownership. This closes the late-entry case in
  `sqlite3_randomness`: guest `0x4236aa` previously decoded through the loop entry at `0x423760`, so
  the function emitted 64 rotates for 32 guest rotates; the final unit emits 32 and moves
  `1,329 -> 1,163` host instructions, versus FEX at 1,015. Exact SQLite `main/10` static-only A/B keeps
  all 2,123 PCs and top-20 roots at 100% coverage, moving `318,921 -> 316,215` (`-2,706`,
  `-0.848486%`). The bounded smallpt shape keeps all 262 PCs and moves `44,634 -> 43,960` (`-674`,
  `-1.510060%`) with 22 shrinking and no growing PCs. Timing-adjusted SQLite output is byte-identical,
  smallpt retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`, and CoreMark retains
  `crcfinal=0x382f`. Mac and Orb pass the replay/function, non-stress direct-link, continuation and SMC
  dependency groups. No stress run, check, diagnostic path or environment switch remains.

- Three adjacent structural candidates were rejected and fully removed. Unconditional physical-next
  region fallthrough exits SQLite incorrectly at `rip=0x51530a`; retaining logical N/Z lazily across
  the compound C/V/AF clear grows the exact SQLite shape `318,921 -> 322,937` (`+1.259246%`); raising
  the region window from 64 to 128 blocks reduces observed units `2,123 -> 1,956` but grows total
  static code `318,921 -> 334,380` because it decodes too many cold blocks.

- `875b49c` routes complete-NZCV backedge materialization through the existing code-region flags
  merge trampoline. The fixed x17 resume ABI preserves x30 continuations and replaces each eligible
  three-instruction inline merge with `ADR + B`; partial masks and configurations without a reachable
  region trampoline retain inline emission. The bounded SQLite `main/1` screen keeps all 2,080 PCs
  and moves `311,544 -> 310,538` host instructions (`-1,006`, `-0.322908%`) with no growing PC.
  `sqlite3_randomness` moves `1,163 -> 1,159`. The bounded smallpt shape keeps all 262 PCs and moves
  `43,960 -> 43,829` (`-131`, `-0.297998%`) with no growth; its PPM SHA-256 remains
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  focused region-flags, shared-merge, continuation-preservation and cycle/SMC groups, and short
  CoreMark retains `crcfinal=0x382f`. No stress run, check, diagnostic path or environment switch
  remains.

- `371d04b` shares repeated direct `CallLambda` targets within a function. Hot call sites now branch
  directly to one function-cold target thunk; the thunk uses the existing fixed-width move-wide
  materialization and tail `BR`, so the helper still returns to the original call site and disk-cache
  relocation remains deterministic. Single-use and dynamic targets retain their inline paths. Against
  the outlined-backedge baseline, bounded SQLite keeps all 2,080 common PCs at 100% coverage and
  moves `310,538 -> 310,506` host instructions (`-32`, `-0.010305%`) with no common-PC growth;
  `sqlite3_randomness` moves `1,159 -> 1,156`. Bounded smallpt keeps all 262 PCs and moves
  `43,829 -> 43,776` (`-53`, `-0.120925%`) with no growth; its PPM SHA-256 remains
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  seven focused CallLambda, disk-cache and AFP host-call cases (134 assertions each), and short
  CoreMark retains `crcfinal=0x382f`. The fingerprint harness confirms cross-process host-byte
  self-consistency over 935 function units, then trips its stale `>3000` unit-count threshold before
  the golden comparison. No stress run, check, diagnostic path or environment switch remains.

- `763cefc` keeps lazy function regions within 8 KiB of their root. More distant direct successors
  remain undecoded external edges and use the existing on-demand L2/direct-link path; eager
  interpreter decoding is unchanged. This prevents split `.cold` sections from being pulled into
  hot units: `read_encoded_value_with_base@0x57c710` no longer embeds 21 blocks rooted at
  `0x401a54` and moves `306 -> 72` host instructions, versus FEX at 69. Against `371d04b`, bounded
  SQLite total static code moves `308,727 -> 304,646` (`-4,081`, `-1.321880%`); three interleaved
  short pairs move wall-clock median `1.368843 -> 1.353392s` and internal median
  `1.139 -> 1.123s`. Bounded smallpt keeps the same 262 roots with no growth and moves
  `43,776 -> 42,990` (`-786`, `-1.795504%`); its PPM SHA-256 remains
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The 4 KiB screen reduced
  more total code but fragmented SQLite from 2,063 to 2,225 roots; the 16 KiB screen left more cold
  code, so neither threshold remains. Mac and Orb pass 302 assertions across function formation,
  SMC, direct-link and disk-cache focuses. CoreMark retains `crcfinal=0x382f`, and the fingerprint
  harness retains cross-process host-byte self-consistency over 935 function units before its stale
  `>3000` threshold. No stress run, check, diagnostic path or environment switch remains.

- `d77ffd8` extends the composite carry-condition fold to the canonical FlagM representation.
  `TestFlags(C) -> TestZero -> And(CondSet(NE))` and the inverse below-or-equal chain now publish
  pending NZCV once, test the packed C/Z pair, and feed the direct `Select` through a local
  EQ/NE condition. This removes the scalar boolean-normalization chain without assuming that the
  pre-fold PSTATE can survive later flag consumers. Against `763cefc`, bounded SQLite keeps all
  2,155 roots and moves `304,646 -> 304,116` host instructions (`-530`, `-0.173972%`) with no
  growth; `sqlite3PagerSetFlags@0x409f00` moves `159 -> 149` and `sqlite3_randomness` moves
  `1,003 -> 997`. Bounded smallpt keeps all 262 roots and moves `42,990 -> 42,832` (`-158`,
  `-0.367527%`) with its canonical PPM SHA-256 unchanged. Mac and Orb pass 121 focused carry,
  CondSet and narrow-branch assertions. A same-host fixed-seed 424242 setcc/cmov/jcc comparison
  retains the baseline's exact 1,214 established differences with zero candidate-only mismatch.
  CoreMark retains `crcfinal=0x382f`. No stress run, check, diagnostic path or environment switch
  remains.

- `3ae5a00` gives REP MOVS/STOS/CMPS/SCAS helpers a narrow resident-SIMD preservation contract.
  The shared guard saves and restores `v16-v23` around the complete helper call, including the
  guest-range callback and libc paths; other live SIMD registers keep the normal per-site capture.
  This avoids the invalid stronger assumption that the helper call graph is general-register-only.
  An exact same-host SQLite `main/10` A/B keeps all 2,087 roots and moves `297,402 -> 296,954`
  host instructions (`-448`, `-0.150638%`) with no growth; `sqlite3_randomness@0x423620` moves
  `997 -> 981`. Bounded smallpt keeps all 262 roots and moves `42,832 -> 42,800` (`-32`,
  `-0.074710%`) with PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Three interleaved
  SQLite pairs are timing-neutral within noise (`1.047817 -> 1.049457s` median). Same-host fixed-seed
  424242 comparisons retain the baseline's exact 126 MOVS and 121 STOS established mismatches and
  zero CMPS/SCAS mismatches. Mac and Orb pass the helper-trait and resident-capture checks; the
  Release wrapper contains only the four expected Q-register save/restore pairs around the helper.
  CoreMark retains `crcfinal=0x382f`. No stress run, check, diagnostic path or environment switch
  remains.

- `4655448` feeds the function CFG's existing fixed-point flags liveness into the destructive flags
  pass when every internal successor has `Flags::None` live-in. Any partial demand, unresolved edge,
  empty successor or host/dispatcher exit keeps the conservative `Flags::All` live-out. This removes
  complete publications that are overwritten before an internal observer without reviving the
  rejected partial-NZCV experiment. ZF-only dead-edge subtraction also accepts a carry inversion
  already removed by the pass; CF-reading conditions still require exactly one inversion. Exact
  SQLite `main/10` keeps all 2,087 roots and versions, moves `296,954 -> 292,116` host instructions
  (`-4,838`, `-1.629209%`), and has 633 shrinking roots with no growth. Three interleaved short pairs
  move wall median `1.047209 -> 1.029851s`; timing-adjusted output is identical. Bounded smallpt
  keeps all 262 roots and moves `42,800 -> 42,204` (`-596`, `-1.392523%`) with PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  function-liveness, dead-edge, SSE4.2 and direct-link/SMC focuses; fixed-seed 424242 setcc/cmov/jcc
  retains the baseline's exact 422 established mismatches with zero candidate-only mismatch.
  CoreMark retains `crcfinal=0x382f`. No stress run, check, diagnostic path or environment switch
  remains.

- `0ed6eb9` lowers the dominant implicit-length `PCMPISTRI 0x1a` form through one AAPCS vector
  helper call. The call path captures live SIMD state, loads the two operands into `q0/q1`, passes
  the control word in `x0`, and skips preserving flags because the instruction overwrites all six
  arithmetic flags. Repeated sites share the existing function-cold host-call target thunk; other
  SSE4.2 string controls retain the inline lowering. Exact SQLite `main/10` keeps all 2,087 roots and
  versions, moves `292,116 -> 291,811` host instructions (`-305`, `-0.104411%`), and has 14
  shrinking roots with no growth. All savings are in `__strcmp_sse42`, which moves `3,949 -> 3,644`
  and closes about 18% of its previous FEX gap. Five interleaved short pairs are timing-neutral within
  noise (`1.086915 -> 1.094917s` median), with identical timing-adjusted output. Bounded smallpt
  stays byte-identical at 42,204 host instructions and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`; CoreMark retains
  `crcfinal=0x382f`. Mac and Orb pass the SSE4.2 differential, scratch, evaluator, CallLambda,
  direct-link and memory-boundary focuses. A cache-compatible two-run SQLite check stores 680 units,
  reloads all 680 on the second process, and keeps adjusted output identical. No stress run, check,
  diagnostic path or environment switch remains.

- `22a0b53` replaces that general AAPCS capture with an exact-clobber ABI for the same hot
  `PCMPISTRI 0x1a` form. A dedicated AArch64 leaf accepts `q0/q1`, returns the packed result in
  `w16`, and names its complete `x10/x11/x13-x17` plus `v0-v7` clobber set; the call emitter saves
  only live members of that set and LR. The generic `EmitHostCall` vector-argument and no-flags
  extensions were removed rather than retained as an unused fallback. The two calls in root
  `0x505120` move from 38/40-instruction frames to 11 each. Exact bounded SQLite keeps all 2,081
  roots and versions, moves `290,734 -> 290,165` (`-569`, `-0.195712%`), and has ten shrinking
  `__strcmp_sse42` roots with no growth. Those roots move `2,646 -> 2,077`; on their exact common
  set FEX is 1,905, so this stage closes 569 of the previous 741-instruction gap. The repeated roots
  are now 195-197 instructions versus FEX at 190; the two remaining larger roots move `325 -> 269`
  and `331 -> 250`. Five interleaved short SQLite pairs improve wall median
  `0.962903 -> 0.953035s` (`-1.025%`) with identical adjusted output. Bounded smallpt remains
  byte-identical at 42,204 instructions with PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`, and CoreMark 20k
  retains `crcfinal=0x382f`. Mac and Orb pass 16,255 Rosetta/SDM assertions, the 512-assertion
  scratch contract, memory-boundary, alias and CallLambda focuses. A cache-compatible two-run
  SQLite check stores and reloads all 680 eligible units with identical output. No stress run,
  check, diagnostic path or environment switch remains.

- The next whole-function audit rejected global approaches for `balance_nonroot`: raising the
  region window grows the full SQLite shape, level-3 GPR pinning reduces that function by 221
  instructions but regresses the five-pair SQLite wall median by 12.054%, and every fixed r13/r15
  home substitution grows total code. The remaining gap requires an explicit cross-block state
  transfer ABI rather than a global pin or decode-window override; none of the audit variants
  remains.

- `PALIGNR` and `VPALIGNR` now share `VecExtractBytes`, a full-width vector primitive lowered to
  one AArch64 `EXT`; offsets beyond one vector reuse the existing zero-backed `VecByteShift`.
  The interpreter and resident-XMM publication proof implement the same operation. On the exact
  2,101-root SQLite `main/10` A/B, total static code moves `292,481 -> 291,437` (`-1,044`,
  `-0.356947%`) with 40 shrinking roots and no growth. `__memmove_ssse3` moves `1,990 -> 1,140`
  (`-850`); its repeated roots move from 62 to 30 instructions, versus FEX at 31, and root
  `0x4f9840` contains the expected three `EXT` instructions. Timing-adjusted SQLite output is
  byte-identical. Mac and Orb pass the 914-assertion resident-XMM test and the complete AVX integer
  reference set. Bounded smallpt remains at 42,204 instructions with PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. No long benchmark,
  check, diagnostic path or environment switch remains.

- Byte `VecMovMask` now uses the same weighted pairwise reduction as FEX. The immutable runtime
  vector prefix holds the repeated `01 02 04 08 10 20 40 80` weights; each lowering is one
  `LDUR`, `CMLT`, `AND`, three `ADDP` instructions and `UMOV`, replacing the previous eight-step
  shift/narrow hierarchy. AES keygen keeps its existing `-16` constant offset, while interrupt
  polling follows the expanded prefix through its named offset. Exact SQLite `main/10` keeps all
  2,101 roots, moves `291,437 -> 291,317` (`-120`, `-0.041175%`), and has 21 shrinking roots with
  no growth. `__strrchr_sse2` moves `675 -> 651`, `__memcmp_sse2` moves `675 -> 660`, and the
  observed `__strcmp_sse42` set moves `1,914 -> 1,903`. Timing-adjusted SQLite output is
  byte-identical. Bounded smallpt moves `42,204 -> 42,140` and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  complete AVX FP2 reference set, the 42-assertion named-constant/AES test and the 26-assertion
  direct-cycle interrupt-poll test. No long benchmark, check, diagnostic path or environment switch
  remains.

- Packed integer `VecCmpEq` and `VecCmpGt` results now enter a dead resident XMM home directly.
  The existing last-use, observer, conflicting-home and emitter-side proofs apply unchanged because
  AArch64 `CMEQ` and `CMGT` are alias-safe three-register operations. This removes the repeated
  post-compare vector copy visible in the remaining glibc string roots. Against the weighted
  movemask baseline, exact SQLite `main/10` keeps all 2,101 roots and moves
  `291,317 -> 291,227` (`-90`, `-0.030894%`) with 11 shrinking roots and no growth.
  `__strrchr_sse2` moves `651 -> 633`, `__memcmp_sse2` moves `660 -> 649`, and root `0x541d60`
  moves `415 -> 398`. Timing-adjusted output is byte-identical. Bounded smallpt moves
  `42,140 -> 42,086` and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  1,034-assertion resident-XMM test and the complete AVX integer reference set. No long benchmark,
  check, diagnostic path or environment switch remains.

- BSF/BSR zero-source destination selection now uses explicit `SelectZero(test, zero, nonzero)` IR.
  ARM64 lowers it to `CMP + CSEL`, the interpreter implements the same choice, and the test source
  remains a named SSA input through register allocation. This replaces the former
  `TestNotZero -> CSET -> CMP -> CSEL` chain without making architectural ZF artificially live.
  A backend-only hidden-source fusion was rejected after SQLite reported a malformed database; it
  allowed RA to reuse the source register before the select and has been removed. Against the
  resident-compare baseline, exact SQLite `main/10` keeps all 2,101 roots and moves
  `291,227 -> 291,138` (`-89`, `-0.030560%`) with 15 shrinking roots and no growth.
  `__strrchr_sse2` moves `633 -> 610` and `__memcmp_sse2` moves `649 -> 635`.
  Timing-adjusted output is byte-identical. Bounded smallpt moves `42,086 -> 42,040` and retains
  PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  Mac and Orb pass the 395-assertion GPR publication proof and the directed BMI/BSF split test;
  1,000 fixed-seed bit-op cases report no BSF/BSR mismatch and only the established ROL/BT
  divergence families. No long benchmark, check, diagnostic path or environment switch remains.

- `4aaf88b` gives REP MOVS/STOS/CMPS/SCAS calls a shared pinned-state ABI instead of expanding the
  generic AAPCS capture at every guest site. The AArch64 wrappers preserve `x3-x15` and
  `v16-v31`; the caller saves only live clobbers, argument cycles and the `x11` host-target
  scratch. The previous `v16-v23`-only C++ guard was removed. Existing `ClampGuestWalk`
  mapped-range checks and guest fault returns remain unchanged. Exact SQLite `main/10` keeps all
  2,215 roots and moves
  `302,834 -> 301,712` host instructions (`-1,122`, `-0.370500%`) with no growth.
  `sqlite3BitvecSet@0x418450` moves `658 -> 602`; its remaining FEX gap is `131` instructions.
  Three interleaved short pairs are timing-neutral within noise: wall medians move
  `1.708 -> 1.712s`, while internal medians move `1.414 -> 1.423s`. Bounded smallpt keeps all 262
  roots, moves `42,040 -> 41,962`, and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  helper metadata, resident-XMM and FPCR focuses. The helper-fault suite retains all REP results;
  its two JIT `fxrstor` scratch-budget failures reproduce on the exact baseline. Fixed-seed 424242
  MOVS/STOS retain the baseline's exact 402/391 mismatch sets with zero candidate-only case, and
  CMPS/SCAS remains clean. No stress run, check, diagnostic path or environment switch remains.

- `da63f32` keeps the same REP helpers under the installed guest FPCR. Their complete call graph is
  integer-only: mapped-range atomics and callbacks plus memcpy/memmove/memcmp operations. The
  existing signal path already restores host FPCR before entering a handler and sigreturn restores
  the interrupted guest value. Pinned-state calls return one result through `x16` when it is not a
  live capture or argument reload; conflicting high-pressure units fail closed to the existing
  stack slot. This removes every per-site FPCR switch/rebuild and the result stack round-trip from
  the hot REP roots. Exact SQLite `main/10` keeps all 2,215 roots and moves
  `301,712 -> 300,650` (`-1,062`, `-0.351991%`) with no growth. `sqlite3BitvecSet@0x418450`
  moves `602 -> 548`, leaving 77 instructions to FEX's 471. Three interleaved short timing pairs
  remain within noise: wall medians `1.669 -> 1.682s`, internal medians `1.393 -> 1.401s`.
  Bounded smallpt keeps all 262 roots, moves `41,962 -> 41,890`, and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Mac and Orb pass the
  helper metadata, resident-XMM and AFP-transparent focuses. Fixed-seed 424242 MOVS/STOS retain
  the baseline's exact 402/391 mismatch sets with zero candidate-only case, and CMPS/SCAS remains
  clean. The REP helper-fault results are unchanged; the two JIT `fxrstor` scratch-budget failures
  still reproduce on the exact baseline. No stress run, check, diagnostic path or environment
  switch remains.

- `d6b584f` treats an `ENDBR64` target other than the current root as a lazy function-region
  boundary. The target remains an ordinary independently compiled L2/direct-link entry; only its
  eager inclusion in the caller's unit is removed. This stops nearby cold functions from being
  mistaken for internal blocks without requiring symbols or a retained heuristic switch.
  `__memcpy_chk@0x514000` drops `136 -> 17` host instructions versus FEX at 29 because its
  `__chk_fail/__fortify_fail` chain is no longer embedded; `printfFunc@0x45ae50` drops
  `586 -> 283`. SQLite `main/10` reshapes 11 old roots into eight new entries, so the exact common
  subset covers 99.564277% of prior static host code and moves `299,340 -> 298,641` (`-699`),
  while the complete total moves `300,650 -> 300,175` (`-475`, `-0.157991%`). Three interleaved
  pairs improve internal median `1.334 -> 1.315s` and wall median `1.604 -> 1.581s`. Bounded
  smallpt reshapes 262 roots into 263 and moves complete total `41,890 -> 41,415` (`-475`,
  `-1.133922%`) with its canonical PPM SHA-256 unchanged. Mac and Orb pass the late-entry replay,
  73-assertion large-function CFG, 38-assertion extracted glibc function, function-liveness and
  35-assertion region-SMC focuses. The counter-based SQLite capture and CoreMark 20k/2k gates did
  not finish inside their 8/6-second caps and were stopped rather than extended; no result is
  claimed from them. No stress run, check, diagnostic path or environment switch remains.

- `2ca20a2` publishes the compound logical flags of a proved self-XOR zero directly. The existing
  self-direct analysis proves the result, so the pending `N=0,Z=1,C=0,V=0,AF=0` word no longer
  executes `MRS NZCV + UBFX + BFI`; it clears the six stored bits and sets Z directly. Other
  logical producers retain the existing PSTATE merge. Exact SQLite `main/10` keeps all 2,212 roots
  and moves `300,175 -> 298,111` (`-2,064`, `-0.687599%`) with no growth. Bounded smallpt keeps
  all 263 roots, moves complete total `41,415 -> 41,251` (`-164`, `-0.395992%`), and retains PPM
  SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Two three-pair
  timing batches reverse sign with ordering: un-warmed
  medians move internal/wall `1.372/1.647 -> 1.382/1.663s`, while warmed reverse-order medians move
  `1.400/1.678 -> 1.386/1.665s`; treat wall as neutral. Fixed-seed 424242 ALU, setcc/cmov/jcc and
  mixed comparisons retain the baseline's exact 1,424/1,214/682 mismatch sets with zero
  candidate-only case. Mac and Orb pass the self-XOR shape and function-liveness focuses. No
  stress run, check, diagnostic path or environment switch remains.

- `ee09012` relies on the acquire-release ordering already carried by aligned scalar LSE atomics.
  CASAL, SWPAL and LDADDAL no longer execute an additional `DMB ISH` before and after the RMW;
  non-LSE exclusive loops and the serialized unaligned basic-access fallback retain both barriers.
  The total static SQLite shape remains exactly 298,111 because those two instructions move into
  the unaligned arm rather than disappearing from the unit. `__run_exit_handlers` contains four
  CASAL and four SWPAL sites, so its aligned path executes 16 fewer DMBs. Three short SQLite pairs
  are timing-neutral within noise: internal/wall medians move `1.358/1.629 -> 1.362/1.637s`.
  `clone_lock_rmw_x86_64` is byte-identical to baseline and exits zero; two AcqRel clone TSO
  litmus runs exit zero with `mp_bad=0`. Fixed-seed 424242 bit-op/CMPXCHG comparison retains the
  baseline's exact 51 mismatch keys with zero candidate-only case. Bounded smallpt remains at 263
  roots / 41,251 instructions with its canonical PPM SHA-256. No stress run, check, diagnostic
  path or environment switch remains.

- `897008d` reserves `x13` across scalar atomic instructions because their unaligned fallback uses
  it for the lock address. Before this fix, a CAS result allocated to `x13` replaced the lock
  pointer while acquiring the lock and issued `STXR` through address 1. The bounded unaligned
  atomic guest now exits zero instead of PageFatal. Exact SQLite remains 2,212 roots / 298,111
  instructions with no changed root; bounded smallpt remains 263 roots / 41,251 instructions and
  retains PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  The focused fixed-clobber test passes 16 assertions. No stress run, check, diagnostic path or
  environment switch remains.

- `040d032` shares repeated 32/64-bit LSE unaligned fallbacks within one translation unit when the
  operation, width and allocated address/result/operand registers all match. A single site remains
  inline, while each repeated site keeps only `ADR + B` and returns through reserved `x13`; the
  cold stub retains the serialized lock, two DMBs and basic faulting memory access. Exact SQLite
  keeps all 2,212 roots and moves `298,111 -> 297,973` (`-138`, `-0.046291%`) with no growth.
  `__run_exit_handlers@0x4e2380` moves `570 -> 536`, reducing its gap to FEX from 115 to 81.
  Bounded smallpt keeps all 263 roots, moves `41,251 -> 41,099` (`-152`, `-0.368476%`) and retains
  PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  Three interleaved SQLite pairs are timing-neutral: internal medians move `1.331 -> 1.343s` and
  wall medians `1.619 -> 1.627s`. Fixed-seed 424242 bit-op/CMPXCHG retains the baseline's exact
  51 mismatch keys with zero candidate-only case. The aligned and unaligned lock-RMW guests exit
  zero on both sides. A temporary two-site unaligned XADD check confirmed two entries sharing one
  cold stub and was deleted. No stress run, diagnostic path or environment switch remains.

- The numbered 1-5 ledger was re-audited against the current `297,973`-instruction SQLite shape.
  Cross-edge carry remains limited by live mixed joins; an N/Z region-trampoline prototype changed
  zero of 2,212 roots and was removed. Return-L1 is already at its current base-ISA
  `LDP + CMP + conditional miss + BLR` continuation contract. Remaining FPR publications are real
  guest-XMM copies or fault captures, and the remaining composite EAs require 32-bit wrap,
  unencodable shifts/scales or bias handling. Do not reopen those pools without a new representation
  contract rather than another local whitelist.

- `7efcadd` fixes the failure that originally blocked a fifteenth static GPR pin. With R13 assigned
  to `x8`, `sqlite3_randomness@0x423620` increased register pressure enough for a dynamic CALL
  target to spill. `TerminalLocationPublication` selected its recovery label during pre-emission
  analysis from one temporary register, while actual emission selected another and failed
  `dynamic_location_miss`; function compilation then fell back to a flat block, producing the
  apparent continuation/stack failure at test 130. Deferred locations now create and share their
  recovery label from the actual emitted target register. The SQLite function compiles normally
  and the bounded Mac `main/10` run completes instead of exhausting the guest stack.

- `d6e9d38` adds R13 in `x8` to the default level-2 map, taking the process ABI from 14 to 15 pinned
  GPRs; R15 remains the only unpinned x86 GPR. A fresh same-input Orb SQLite short capture keeps all
  2,083 roots and moves `285,245 -> 281,278` (`-3,967`, `-1.390727%`). Bounded smallpt keeps all
  263 roots, moves `41,099 -> 40,813` (`-286`, `-0.695881%`) and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Applying the retained
  SQLite weights to the covered `main/10` subset gives `-1.268906%` weighted host instructions at
  23.569162% formal host-weight coverage; this is directional evidence, not a formal promotion
  result. Three interleaved short SQLite pairs regress wall median `1.115 -> 1.139s` and internal
  median `0.871 -> 0.893s`, so the change improves the instruction-quality target but is not a
  demonstrated end-to-end speedup. Mac and Orb focused direct-link, continuation, indirect-call
  and static-pin groups pass 225 assertions. No stress run, check, diagnostic path or environment
  switch remains.

- `df82e5b` removes the whole-unit Linux x18 spill reservation that amplified the register pressure
  from the fifteenth pin. Scalar spill reloads and definitions now take x18 only when it is free at
  the current instruction; the verified reload headroom handles conflicts and further temporaries.
  Adjacent spill forwarding also checks the consumer's active GPR mask and marks a forwarded x18
  live before emitter scratch allocation. The obsolete allocation rerun and unit-reservation API
  are removed. A fresh same-command static SQLite A/B keeps all 2,171 roots and moves
  `290,394 -> 290,165` (`-229`, `-0.078858%`); `sqlite3_randomness@0x423620`, which exposed the
  spill avalanche, moves `1,225 -> 1,004` (`-221`). Three interleaved short pairs move wall median
  `1.080353 -> 1.062930s`; the internal median `0.794 -> 0.799s` remains within short-run noise.
  Bounded smallpt `4 8 6` exits zero and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The saturated scratch
  test passes 763 Mac and 779 Orb assertions; the dedicated free/conflicting-x18 forwarding cases
  pass 2 Mac and 9 Orb assertions. No stress run, check, diagnostic path or environment switch
  remains.

- The refreshed same-input SQLite/FEX attribution still puts `balance_nonroot` first: SwiftVM
  reaches it through 28 lazy roots covering 316 decoded blocks and emits 3,799 instructions,
  while FEX emits one 3,087-instruction multiblock unit. Raising the region window remains invalid:
  the existing 64-to-128 audit reduced unit count but compiled enough cold blocks to grow total
  SQLite code. R15 accounts for 138 SwiftVM state loads/stores in those roots, but every examined
  outgoing carry candidate crossed a guest-fault observation after its last safe R15 SSA value.
  Passing that value directly would make signal return restore State while translated code retained
  a stale register. No cross-edge prototype or diagnostic path was retained.

- `16de8d5` fuses the largest recurring flags-publication sequence found by the same audit. When a
  logical result has a retained parity token and the adjacent clear completes the compound NZ/CV/AF
  update, x26's final live representation is exactly current NZCV plus the low parity byte. The
  emitter now builds that representation with `MRS x26,NZCV + BFI` instead of publishing the token
  and then extracting/reinserting bits 26-31 through a scratch register. Tokenless partial updates
  retain the preserving merge. Exact SQLite keeps all 2,171 roots and moves
  `290,165 -> 289,189` (`-976`, `-0.336360%`) with no growth. Bounded smallpt keeps all 263 roots,
  moves `40,809 -> 40,697` (`-112`, `-0.274449%`) and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Five interleaved SQLite
  pairs move wall median `1.078718 -> 1.076245s`; internal median `0.814 -> 0.817s` remains within
  short-run noise. Timing-adjusted SQLite output is byte-identical, CoreMark 20k retains
  `crcfinal=0x382f`, and Mac/Orb logical-flags focuses pass 21 assertions each. Fixed-seed 424242
  ALU differential retains the baseline's exact 182 mismatch keys with zero candidate-only case.
  No stress run, check, diagnostic path or environment switch remains.

- `3dc5157` removes the next packed-flags duplication. For an exact CV/AF clear with N/Z still
  pending in host PSTATE and a complete parity token, every old x26 field is either explicitly
  cleared, pending in PSTATE or replaced by that token. One `UXTB w26,wToken` therefore replaces
  the separate `BFC x26,#26,#4 + BFXIL x26,token,#0,#8` publication without changing carry
  polarity or crossing an observation boundary. Exact SQLite keeps all 2,171 roots and moves
  `289,189 -> 287,411` (`-1,778`, `-0.614823%`) with no growth. Bounded smallpt keeps all 263 roots,
  moves `40,697 -> 40,469` (`-228`, `-0.560238%`) and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. Five interleaved SQLite
  pairs move wall median `1.107450 -> 1.064690s` and internal median `0.842 -> 0.796s`; treat these
  short timings as a consistency check, not a throughput claim. Timing-adjusted SQLite output is
  byte-identical, CoreMark 20k retains `crcfinal=0x382f`, and Mac/Orb logical-flags focuses pass
  23 assertions each. Fixed-seed 424242 ALU differential retains the baseline's exact 182 mismatch
  keys with zero candidate-only case. No stress run, check, diagnostic path or environment switch
  remains.

- `6dc8e14` folds incoming packed `JA/JBE` conditions without reconstructing their boolean graph.
  When carry is no longer live in PSTATE, the canonical FlagM path now commits any pending NZCV and
  tests x26's guest CF/ZF bits directly; the live-PSTATE direct-carry path keeps `HI/LS`. Exact
  SQLite keeps all 2,171 roots and moves `287,411 -> 287,236` (`-175`, `-0.060888%`) with no growth.
  Bounded smallpt keeps all 263 roots, moves `40,469 -> 40,412` (`-57`, `-0.140849%`) and retains
  PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  Timing-adjusted SQLite output is byte-identical, CoreMark 20k retains `crcfinal=0x382f`, and the
  Mac/Orb packed/direct carry focuses pass 106 assertions each. Fixed-seed 424242 JCC differential
  retains the baseline's exact 107 mismatch keys with zero candidate-only case. No stress run,
  check, diagnostic path or environment switch remains.

- `d490189` removes the production indirect-L1 value-zero comparison. Production publishes the
  value before its key and replaces an invalidated key hit with the nonzero shared miss trampoline;
  only the diagnostic profiler retains zero as a distinct miss value. A matching production key is
  therefore already branch-safe, so `CMP + CCMP` becomes one `CMP` while key mismatches retain the
  same miss/continuation paths. Exact SQLite keeps all 2,170 roots and moves `287,228 -> 286,301`
  (`-927`, `-0.322740%`) with no growth. Bounded smallpt keeps all 263 roots, moves
  `40,412 -> 40,235` (`-177`, `-0.437989%`) and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. CoreMark keeps all
  294 roots, moves `40,489 -> 40,310` (`-179`, `-0.442095%`) and retains `crcfinal=0x382f`.
  Timing-adjusted SQLite output is byte-identical; three interleaved short pairs move wall median
  `1.032095 -> 1.014797s` and internal median `0.761 -> 0.756s`. Mac and Orb L1/SMC/interrupt
  focuses pass 59 assertions each. No stress run, check, diagnostic path or environment switch
  remains.

- The numbered 1-5 ledger was re-audited after the packed-branch and production-L1 stages. Full
  Linux GPR pinning reduces static SQLite `287,228 -> 284,162` (`-1.067445%`), smallpt
  `40,412 -> 40,172` (`-0.593883%`) and CoreMark `40,489 -> 40,215` (`-0.676727%`), but grows
  126 SQLite roots by 391 instructions and regresses three-pair wall/internal medians
  `1.059766/0.796 -> 1.089001/0.808s`; Apple also lacks the extra safe scratch slot for the
  high-pressure full-pin ABI. The default flip was fully removed. A late x14 call-continuation
  handoff changes zero of 2,171 roots because every feasible return value is already assigned x14.
  A fallthrough-first 64-block region queue changes unit formation, covers only 86.35% of the old
  static shape and grows its common subset by 0.897911%; it was removed. Remaining FPR publications
  are guest-XMM copies or fault captures, and the remaining composite EAs already use the direct
  `UXTW` path when their 32-bit wrap, bias and scale are encodable. No prototype, diagnostic source
  path or new environment switch remains.

- `58fb5ee` collapses the complete known-zero compound logical-flags representation. A self-XOR
  defines N/C/V/AF as zero, Z as one and the retained parity byte as zero, so x26 is exactly the Z
  bit regardless of whether the result token is still available. One constant publication now
  replaces the tokenless `BFC + ORR` update and omits the redundant token insertion. A strict paired
  SQLite capture keeps all 2,167 roots and moves `285,599 -> 283,516` (`-2,083`, `-0.729344%`) with
  no growth. Bounded smallpt keeps all 263 roots, moves `40,235 -> 40,070` (`-165`, `-0.410091%`)
  and retains PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  CoreMark keeps all 294 roots, moves `40,310 -> 40,131` (`-179`, `-0.444059%`) and retains
  `crcfinal=0x382f`. Timing-adjusted SQLite output is byte-identical; three interleaved short
  pairs move wall median `1.098 -> 1.090s` and internal median `0.831 -> 0.816s`. Mac and Orb
  logical-flags focuses pass 27 assertions each. Fixed-seed 424242 with 256 ALU iterations retains
  the baseline's exact 100 mismatch keys with zero candidate-only case. No stress run, check,
  diagnostic path or environment switch remains.

- `4d8e1f0` moves paired integer-helper preservation out of every Div128 and CPUID call site into
  one AddressSpace trampoline. The site records its three inputs, helper target, x11/x16/LR and two
  outputs in a fixed 64-byte frame; the shared trampoline preserves caller-saved GPRs once and calls
  the existing exact helper. Compilers without the general-register-only contract also preserve
  SIMD state in that shared code. A strict paired SQLite capture keeps all 2,166 roots and moves
  `283,508 -> 282,881` (`-627`, `-0.221158%`) with no growth. The three largest CPUID roots lose
  117, 63 and 46 instructions; `pagerFlushOnCommit` and `sqlite3BtreeSetSpillSize` each lose 21.
  Bounded smallpt keeps all 263 roots, moves `40,070 -> 39,746` (`-324`, `-0.808585%`) and retains
  PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  CoreMark keeps all 294 roots, moves `40,131 -> 39,819` (`-312`, `-0.777454%`) and retains
  `crcfinal=0x382f`. Timing-adjusted SQLite output is byte-identical; three interleaved short
  pairs keep wall median `1.025 -> 1.025s` and move internal median `0.772 -> 0.771s`. Mac and Orb
  fixed seeds 101/424242 pass 100 DIV/IDIV iterations, CPUID passes 29 assertions, and Orb pin
  levels 0-3 each pass a 30-iteration DIV/IDIV check. No stress run, check, diagnostic path or
  environment switch remains.

- `218383a` combines cold cycle-exit flags publication with the shared halt-reason path. A full-NZCV
  exit previously branched through an outlined merge, resumed locally and then branched again to
  the reason tail. New region-trampoline entries preserve AF, optionally insert the parity token
  and continue directly into CodeMiss/Signal selection, leaving one branch at each eligible stub.
  A strict paired SQLite capture keeps all 2,167 roots and moves `282,889 -> 281,959` (`-930`,
  `-0.328751%`) with no growth. Bounded smallpt keeps all 263 roots, moves `39,746 -> 39,656`
  (`-90`, `-0.226438%`) and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. CoreMark keeps all
  294 roots, moves `39,819 -> 39,705` (`-114`, `-0.286295%`) and retains `crcfinal=0x382f`.
  Timing-adjusted SQLite output is byte-identical; six short interleaved/reverse-order pairs move
  wall median `1.051 -> 1.043s` and internal median `0.787 -> 0.779s`, used only as a consistency
  check. Mac and Orb pass 49 cycle assertions and 93 direct-link/flags assertions each. A separate
  block-local known-zero-AF merge prototype saved only 16 SQLite instructions, while a matching
  CheckHalt merge-to-return entry changed zero roots; both were fully removed. No stress run, check,
  diagnostic path or environment switch remains.

- `ab19937` combines a terminal full-NZCV publication with the host-return trampoline. Eligible
  cold exits previously emitted `ADR + merge branch`, resumed locally and then emitted a second
  return branch. The terminal now consumes only the immediately preceding merge recipe, removes
  its pending patch record, rewinds that sequence and emits one branch to a shared merge-and-return
  entry. AF remains in x26 and the token entry inserts parity from x12. Direct-link recipes already
  consumed by a link site and the deferred-BL form are unchanged. A strict paired SQLite capture
  keeps all 2,167 roots and moves `281,959 -> 274,241` (`-7,718`, `-2.737277%`) with no growth.
  Bounded smallpt keeps all 263 roots, moves `39,656 -> 38,762` (`-894`, `-2.254388%`) and retains
  PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  CoreMark keeps all 294 roots, moves `39,705 -> 38,735` (`-970`, `-2.443017%`) and retains
  `crcfinal=0x382f`. Timing-adjusted SQLite output is byte-identical; three short
  interleaved/reverse-order pairs keep wall median `1.022 -> 1.022s` and move internal median
  `0.769 -> 0.771s`, used only as a consistency check. Mac and Orb pass 61 cycle assertions,
  25 return/flags assertions and 105 direct-link/flags assertions each. No stress run, check,
  diagnostic path or environment switch remains.

- `ae6471c` replaces the block-wide ADC/SBB flags-elimination bailout with guest-region protection.
  `flags_carry_regions` partitions IR at `AdvancePC`, marks each carry consumer and walks backwards
  through intervening regions to the actual carry writer. The flags pass resets both its needed
  mask and local-label captures at those barriers while continuing ordinary dead-write elimination
  in unrelated regions. `SVM_FLAG_CARRY_ELIM=0` retains the exact whole-block bailout. A strict
  paired SQLite capture keeps all 2,167 roots and moves `274,241 -> 273,723` (`-518`,
  `-0.188885%`) with no growth; `powerOfTen` moves `242 -> 199` and `sqlite3BitvecSet` moves
  `516 -> 436`. Bounded smallpt keeps all 263 roots and moves `38,762 -> 38,713` (`-49`,
  `-0.126412%`) while retaining PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. CoreMark keeps all
  294 roots, moves `38,735 -> 38,686` (`-49`, `-0.126501%`) and retains `crcfinal=0x382f`.
  Timing-adjusted SQLite output is byte-identical; three short interleaved pairs move wall median
  `1.013 -> 1.006s` and internal median `0.762 -> 0.753s`, used only as a consistency check. Mac
  and Orb pass 54 flag-elimination assertions and 57 carry assertions each. Fixed-seed 424242 ALU
  retains the baseline's exact 100 mismatch keys, and mixed seed 101 retains all 105 keys exactly.
  Mixed seed 424242 reports two nominal candidate-only keys in blocks containing no ADC/SBB; their
  baseline/candidate AArch64 streams have identical instruction counts, mnemonics and adjusted
  operands, differing only in the ASLR-derived SetLocation immediate. The unsafe unrestricted
  bailout removal and the one-instruction adjacent-chain prototype were fully removed. No check,
  diagnostic path or environment switch remains.

- The post-`ae6471c` FEX refresh leaves the largest positive SQLite root gaps at `freeSpace`
  `436/309`, `__strcspn_sse42` `326/241`, `__memcmp_sse2` `626/547`, `_IO_new_file_xsputn`
  `460/383`, `setupLookaside` `405/333` and `pagerPagecount` `238/177` (SwiftVM/FEX).
  `powerOfTen` falls from an 89-instruction gap to 46 and is no longer a top-five root. The next
  tranche should attack the repeated cycle/link boundaries in `freeSpace` and then separate the
  libc string-memory lowering gaps. The old two-instruction BFXIL merge, global carry polarity,
  larger region and full-pin attempts remain measured regressions.

- The `freeSpace` boundary audit rules out the 64-block region cap as its main cause. Its unit emits
  37 guest blocks and ten fault-backed cycle polls: one is the internal DFS backedge and nine are
  static cut edges to three entries inside blocks decoded earlier (`0x4498c9`, `0x449a5b` and
  `0x449b0c`); seven error paths share `0x449b0c`. A cross-frontier HIR replay prototype reduced
  exact SQLite `273,723 -> 270,992`, smallpt `38,713 -> 38,457` and CoreMark `38,686 -> 38,434`,
  with byte-identical SQLite output, the canonical smallpt image and `crcfinal=0x382f`. Repeated
  identical SQLite runs later exposed nondeterministic `malloc_consolidate` corruption, so the
  builder, test and CMake entry were fully removed. Inferring interior region edges from trailing
  `SetLocation`, even when restricted to direct `E9/EB` joins or a 16-byte duplicate-tail threshold,
  separately produced guest Signal exits or heap corruption; those variants were also removed.
  The restored Orb translator is byte-identical to the `ae6471c` baseline
  (`ef3548a9d472a15c5444ecf3e759f88df88023bdaed9dce6c743646ffcf32c4c`) and returns to 2,167
  roots / 273,723 instructions. Do not retry heuristic replay or `SetLocation` internalization.
  Closing this `freeSpace` pool requires an explicit split-entry ownership contract that preserves
  overlapping x86 streams, call-return ownership and public-entry recovery semantics.

- A function/region-local R15 carrier in `x9` was rejected and fully removed. State write-through,
  public-entry reloads and block-local epochs each still produced a deterministic SQLite guest halt
  around `sqlite3BtreeOpen`/`setSectorSize`; replacing the suppressed reload with the real load
  passed. The restored translator is byte-identical to the baseline
  (`ef3548a9d472a15c5444ecf3e759f88df88023bdaed9dce6c743646ffcf32c4c`) and an exact paired
  capture keeps all 2,166 roots at 273,715 instructions. Do not retry heuristic R15 residency;
  it needs explicit split-entry state ownership and fault metadata.

- Full-width dead carry normalization is now removed in non-entry HIR blocks. For direct U32/U64
  subtraction, the adjacent `InvertCarry` changes only Carry, so the existing successor-liveness
  proof can discard it when the branch does not consume Carry; public function entries remain
  excluded because they can otherwise enlarge the pending-flags call-entry veneer. Exact paired
  SQLite keeps all 2,167 roots and moves `273,723 -> 273,030` (`-693`, `-0.253176%`), with 187
  shrinking and 42 growing roots. Bounded smallpt keeps all 263 roots and moves
  `38,713 -> 38,657` (`-56`, `-0.144654%`), with 14 shrinking and five growing roots; its PPM
  SHA-256 remains `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  CoreMark 2k keeps all 295 roots, moves `38,696 -> 38,631` (`-65`, `-0.167976%`) and retains
  `crcfinal=0x4983`. Three interleaved short SQLite pairs move wall/internal medians
  `1.013968/0.757 -> 1.000986/0.751s`; all six timing-adjusted outputs are byte-identical.
  Mac and Orb pass the dedicated two-case/nine-assertion focus and the broader
  nine-case/126-assertion flag focus. Fixed-seed 424242, 256-case ALU and setcc/cmov/jcc
  comparisons retain the baseline's exact 79 and 123 established mismatch sets with zero
  candidate-only mismatch. No stress run, check, diagnostic path or environment switch remains.

- The explicit split-entry audit confirmed `0x4498c9`, `0x449a5b` and `0x449b0c` are real x86
  instruction boundaries, but boundary direct is still insufficient. A prototype transferred
  HIR ownership, call-return metadata, CFG edges and guest-code dependencies only at exact
  `AdvancePC` cuts, rejected prefix SSA use and incoming-flags observation, and retained
  `CheckHalt` on internalized cycle edges. Isolating it to `freeSpace` moved that root
  `417 -> 399`, but global activation reproduced guest halt in libc initialization and heap
  corruption in `_int_malloc`; fan-in thresholds only changed which function failed. The entire
  implementation, test and diagnostic output were removed. A viable split entry needs a frontend
  decoder-state capture in addition to HIR/fault ownership; do not infer it from `SetLocation`,
  exact instruction boundaries or fan-in counts.

- Implicit-length PCMPISTRI control `0x02` now uses a shared vector helper ABI instead of expanding
  equal-any aggregation in every JIT unit. The AArch64 entry marshals `v0/v1` to the existing
  evaluator and returns the packed result in `x16`; native `0x1a` and generic `0x02` helpers retain
  separate caller-clobber masks. The rejected `0x3a` extension grew nine roots by 18 instructions
  each and was fully removed. Two repeated exact SQLite captures keep all 2,167 roots and move
  `273,030 -> 272,988` (`-42`, `-0.015383%`) with one shrinking root and no growth:
  `__strcspn_sse42` moves `326 -> 284`, narrowing its FEX gap from 85 to 43 instructions.
  Three interleaved short pairs move wall/internal medians
  `1.032009/0.774 -> 1.025786/0.769s`; all six timing-adjusted outputs are byte-identical.
  Bounded smallpt is byte-identical at 263 roots / 38,657 instructions with PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. CoreMark 2k is
  byte-identical at 295 roots / 38,631 instructions and retains `crcfinal=0x4983`. Mac and Orb pass
  16,800 assertions across the helper scratch contract, SDM/Rosetta differential, memory-boundary,
  alias/REX and evaluator checks. No stress run, check, diagnostic path or environment switch
  remains.

- The remaining continuation return hot path is at its current safe ABI floor. Each return loads
  the architectural target from the guest stack, loads the predicted target/host continuation pair
  from the guarded x25 stack, compares the guest targets, branches to the shared miss path and uses
  `BLR` so a null/stale continuation fault can recover through LR-based metadata. Replacing that
  sequence with FEX's single `RET` requires a different hardware-stack ownership and invalidation
  ABI; no return peephole or speculative fallback was retained.

- Memory CMPXCHG now publishes the observed CAS value directly to the accumulator. On success the
  observed value equals the expected accumulator, and on failure x86 requires that same observed
  value, so the previous `XOR + TestZero + Select` was redundant. The comparison flags are marked
  as the immediately following instruction's local NZCV, allowing existing branch-only analysis
  to retain `SUBS` while deleting dead parity/AF/carry construction. Register-destination CMPXCHG
  keeps its conditional destination and accumulator selections. Exact SQLite keeps all 2,167 roots
  and moves `272,988 -> 272,701` (`-287`, `-0.105133%`) with 18 shrinking roots and no growth;
  `version_lock_lock_exclusive` moves `194 -> 168`, narrowing its FEX gap from 69 to 43.
  Bounded smallpt keeps all 263 roots, moves `38,657 -> 38,386` (`-271`, `-0.701037%`) with 17
  shrinking roots and no growth, and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. CoreMark 2k keeps all
  295 roots, moves `38,631 -> 38,344` (`-287`, `-0.742927%`) with 17 shrinking roots and no
  growth, and retains `crcfinal=0x4983`. Three interleaved SQLite pairs keep wall median neutral
  (`1.022901 -> 1.022955s`) and move internal median `0.770 -> 0.766s`; all six adjusted outputs
  are byte-identical. Fixed-seed 424242, 256-case Mac and Orb bit-op differentials report zero
  CMPXCHG mismatch; their only failures are the established ROL family. No stress run, check,
  diagnostic path or environment switch remains.

- `Div128NarrowingAnalysis` now preserves the existing native `SDIV/UDIV + MSUB` path when a
  standard narrow dividend crosses pinned architectural state. It resolves only exact same-block
  `StoreUniform` to `LoadUniform` chains and basic operand wrappers; overlapping writes,
  `UniformBarrier`, host calls, x87 and SSE4.2 helpers stop the proof. This recovers `CQO; IDIV`
  pairs whose RDX sign extension was previously hidden from the backend while arbitrary 128-bit
  dividends retain the paired helper. Exact SQLite keeps all 2,167 roots and moves
  `272,701 -> 272,312` (`-389`, `-0.142647%`) with 18 shrinking roots and no growth.
  `sqlite3BtreeSetSpillSize` moves `133 -> 100`, narrowing its FEX gap from 69 to 36, and
  `setupLookaside` moves `403 -> 373`, narrowing its gap from 70 to 40. The bounded
  `smallpt_wh_x64 4 8 6` screen keeps all 263 roots, moves `38,386 -> 38,371`, and retains PPM
  SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The shortened
  CoreMark screen keeps all 294 reached roots, moves `38,334 -> 38,319`, and retains
  `crcfinal=0x382f`. Mac and Orb fixed-seed 424242 DIV/IDIV screens pass 256 iterations; Orb pin
  levels 0 through 3 also pass fixed-seed 101 at 100 iterations with zero mismatch. SQLite's
  timing-adjusted output is byte-identical. No stress run, check, diagnostic path or environment
  switch remains.

- U32 `VecMovMask` results are now eligible for the existing fixed-GPR publication transaction.
  The allocator reuses the architectural home only after its normal last-use, target-conflict,
  observer and live-range checks succeed, allowing PMOVMSKB/MOVMSK results to omit the separate
  `ZeroExtend32To64` publication move. Extending width-component ownership produced no additional
  change and was removed. Exact SQLite keeps all 2,167 roots and moves `272,312 -> 272,251`
  (`-61`, `-0.022401%`) with 18 shrinking roots and no growth. `__strrchr_sse2` moves
  `595 -> 584`, narrowing its FEX gap from 68 to 57; `__memcmp_sse2` moves `626 -> 620`, narrowing
  its gap from 79 to 73. `__strchr_sse2`, `__strchrnul_sse2`, `__strlen_sse2` and
  `__strnlen_sse2` lose eight, eight, eight and seven instructions. The bounded smallpt screen
  moves `38,371 -> 38,336` with four shrinking roots and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The shortened CoreMark
  screen moves `38,319 -> 38,292` with three shrinking roots and retains `crcfinal=0x382f`.
  Mac and Orb pass 129 SSE edge assertions, the AVX movemask reference and 30 assertions across ten
  pinned-GPR cases. SQLite's timing-adjusted output is byte-identical. No stress run, check,
  diagnostic path or environment switch remains.

- `RawCarryBranchAnalysis` now proves exact same-block unsigned compare branches whose
  `SaveFlags(Sub/Sbb) -> InvertCarry -> AdvancePC -> TestFlags(Carry)` chain feeds only JA/JBE.
  The translator keeps the raw ARM subtraction carry in PSTATE for the terminal `HI`/`LS`
  condition, merges it into the architectural flags word and flips only the committed carry bit.
  This removes one `CFINV` from every matched chain without changing the continuation or flags ABI.
  Exact SQLite keeps all 2,167 roots and moves `272,251 -> 272,049` (`-202`, `-0.074196%`) with
  145 shrinking roots and no growth; repeated candidate captures are identical. Bounded smallpt
  keeps all 263 roots, moves `38,336 -> 38,268` (`-68`, `-0.177379%`) with 39 shrinking roots and
  no growth, and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The shortened CoreMark
  screen moves `38,292 -> 38,224` (`-68`, `-0.177583%`) with 39 shrinking roots, no growth and
  `crcfinal=0x382f`. Mac and Orb pass the directed JA/JBE semantics, branch-only flags,
  flags-liveness and extracted 72-block glibc focuses. An attempted hot-block trace scheduler was
  fully removed after the extracted glibc function failed: the current emitter places per-block
  cold stubs immediately after hot terminals, so arbitrary hot-block reordering cannot preserve
  fallthrough until hot and cold emission are separated. No stress run, check, diagnostic path or
  environment switch remains.

- `FunctionDecodeFrontier` now resolves late split entries by rerunning the x86 frontend instead of
  transferring existing HIR. A target is internalized only when it is local to the function, lies
  inside exactly one decoded owner, that owner can be reset without call-return ownership, and a
  fresh decoder run ends exactly at the target. Ambiguous, mid-instruction, ENDBR64, nonlocal and
  non-resettable entries remain external. Accepted targets may supersede an existing code-cache
  boundary because the new owner, terminal, guest-code dependencies and call-return metadata are
  all regenerated by the frontend. This avoids the stale state that invalidated the earlier HIR
  replay prototype. The first CallLambda integration exposed a separate cold-path scratch contract
  hole: partial noncontiguous NZCV merges could ask VIXL for an implicit register after the cold
  pool had been closed. Terminal/cold merges now materialize those masks through an explicit fixed
  scratch; ordinary hot merges are unchanged.

  Two final SQLite runs have identical common roots and adjusted output; runtime reachability of
  one optional eight-instruction root changes the total between `269,365` and `269,373`. Against
  the `272,049` baseline the conservative reduction is 2,676 instructions (`-0.983644%`).
  `__printf_buffer@0x529bc0` moves `287 -> 264`, `_IO_new_file_xsputn` `457 -> 435`,
  `__memcmp_sse2` `614 -> 592` and `setupLookaside` `373 -> 347`; `freeSpace` remains unchanged at
  417 versus FEX at 309. Bounded smallpt keeps all 263 roots, moves `38,268 -> 37,998`
  (`-0.705550%`) and retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`. The shortened CoreMark
  screen keeps 294 roots, moves `38,224 -> 37,979` (`-0.640958%`) and retains
  `crcfinal=0x382f`. Mac and Orb each pass 180 assertions across CallLambda/static interaction,
  large function JIT, the extracted 72-block glibc function, SMC dependency ownership and unsigned
  carry semantics. Extending raw carry directly to JB/JAE was rejected and fully removed: preserving
  the canonical flags ABI added `MergeNZCV + EOR` around a one-instruction `CFINV`, growing SQLite
  `272,049 -> 272,533` (`+0.177909%`). No diagnostic source, environment switch or stress path
  remains.

- Unconditional direct-jump discovery was audited and fully removed. Exposing every constant jump
  as a function `LinkBlock` first caused an early deterministic SQLite host fault; restricting it to
  backward or unique-owner targets moved the failure to heap corruption and made the bounded
  smallpt image uniformly red. Keeping the direct source external did not repair the contract:
  retaining the split target in the same HIR CFG still produced wild-PC smallpt exits, a SQLite
  guest halt and zero CoreMark CRCs. The restored tree reproduces the exact accepted baseline:
  SQLite `2,164 / 269,373`, smallpt `263 / 37,998` with SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`, and CoreMark
  `294 / 37,979` with `crcfinal=0x382f`. Closing the remaining `freeSpace` direct entries therefore
  needs an explicit external-edge or multiple-CFG-root state ABI; no check, edge flag, target
  registry or address gate remains.

- 32-bit bit scans and counts now retain their architectural width through dedicated
  `CountLeadingZeros32` / `CountTrailingZeros32` IR operations. A shared frontend adjustr masks
  only the misreported 16-bit form; 32/64-bit BSF, BSR, LZCNT and TZCNT no longer create direct
  all-ones masks, and 32-bit counts use native AArch64 `CLZ W` or `RBIT W + CLZ W` instead of the
  64-bit scan/helper path. The same-input formal SQLite diff has 100% host and entry coverage, keeps
  all 2,164 roots and moves `269,373 -> 269,235` (`-138`, `-0.051230%`) with no growth.
  `__strrchr_sse2` moves `583 -> 535` versus FEX at 527, and `__memcmp_sse2` moves `592 -> 558`
  versus FEX at 547. Two final SQLite shapes and timing-adjusted outputs are identical. Bounded
  smallpt moves `37,998 -> 37,920` with the same PPM SHA; CoreMark moves `37,979 -> 37,908` and
  retains `crcfinal=0x382f`. Mac and Orb pass the BMI-enabled 27-assertion width/encoding matrix.
  No stress run, diagnostic path or environment switch remains.

- Narrow DIV/IDIV remainder construction now maps directly to the host instruction shape. The
  zero-denominator guard selects on the denominator itself through `SelectZero`, and the quotient
  remainder expression uses a shared `MulSub` IR operation lowered to AArch64 `MSUB` instead of
  separate multiply and subtract instructions. The same-input SQLite diff retains all 2,164 roots,
  has 100% host and entry coverage, and moves `269,235 -> 269,006` (`-229`, `-0.085056%`) with no
  growth. `pcache1TruncateUnsafe` moves `199 -> 190` versus FEX at 141, and its emitted root contains
  the expected three `MSUB` instructions. The `SelectZero` part accounts for 154 instructions and
  `MulSub` removes another 75. Two final SQLite shapes are byte-identical and their timing-adjusted
  output matches both each other and the baseline. Bounded smallpt moves `37,920 -> 37,917` with the
  same PPM SHA; CoreMark moves `37,908 -> 37,904` and retains `crcfinal=0x382f`. Mac and Orb pass the
  fixed-seed 256-iteration DIV/IDIV differential. No dedicated check or debug switch was added.

- A block-final `CondSet` whose only consumer is the terminal branch now retains its condition in
  live host NZCV instead of materializing a boolean for a following `CBZ` or `CBNZ`. The gate requires
  both `save_in_nzcv` and dirty NZCV, and the existing `RecordLocalCondition` proof rechecks the sole
  terminal use before the region terminal emits `B.cond`. The same-input SQLite diff retains all
  2,164 roots with 100% host and entry coverage, moves `269,006 -> 268,174` (`-832`, `-0.309287%`),
  and shrinks 439 roots with no growth. `sqlite3GetVarint` moves `231 -> 225` versus FEX at 181;
  `__strrchr_sse2` moves `535 -> 528` versus FEX at 527. Two final SQLite shapes are byte-identical
  and their timing-adjusted output matches the baseline. Bounded smallpt moves `37,917 -> 37,785`
  with the same PPM SHA; CoreMark moves `37,904 -> 37,759` and retains `crcfinal=0x382f`. Mac and Orb
  pass 206 focused CondSet, region-flags and dead-edge assertions. The one-shot IR dump used to
  identify the terminal shape was deleted; no diagnostic path or environment switch remains.

- Adjacent single-use `LSR/ASR -> AND #1` graphs now emit one `UBFX` from the original source. The
  matcher is isolated with the other narrow-extract recipes and requires matching scalar widths, no
  pseudo flags, an in-range immediate, a real unspilled source allocation and exact adjacency. The
  same-input SQLite diff keeps all 2,164 roots with 100% host and entry coverage, moves
  `268,174 -> 267,957` (`-217`, `-0.080918%`), and shrinks 90 roots with no growth.
  `pcache1TruncateUnsafe` moves `190 -> 188` versus FEX at 141, while `powerOfTen` moves `202 -> 200`
  versus FEX at 153. Two final SQLite shapes are byte-identical and their timing-adjusted output
  matches the baseline. Bounded smallpt moves `37,785 -> 37,745` with the same PPM SHA; CoreMark
  moves `37,759 -> 37,717` and retains `crcfinal=0x382f`. Mac and Orb pass 273 focused shift,
  narrow-extract and width-chain assertions. No new test, check or debug switch remains.

- Function-level decoding now internalizes an unconditional constant jump only when its target is
  already a block in the current HIR function. The existing block proves that another decoded edge
  already established the target boundary and code-object ownership; unknown targets, indirect
  targets and newly discovered split targets retain the canonical dispatcher exit. The runtime
  audit confirmed that every decoded block already has separate external/direct/pending-flags/call
  entries and participates in the code object's SMC ownership transaction, so no duplicate entry
  contract or veneer layer was added. On the short SQLite main screen, all 2,075 baseline roots are
  retained and move `259,747 -> 255,517`; 72 new jump-source boundary roots add 286 instructions,
  leaving the complete candidate at 2,147 roots / 255,803 instructions (`-3,944`, `-1.518368%`).
  The top 20 roots are all covered with no growth, repeated candidate shapes are identical,
  timing-adjusted output is byte-identical and `freeSpace` moves `416 -> 409`. Bounded smallpt
  moves `37,745 -> 37,132`, retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`, and CoreMark 20k moves
  `37,717 -> 37,160` with `crcfinal=0x382f`. Mac and Orb pass the directed internal/external jump
  boundary, late-split replay, region ownership, CallLambda, 72-block CFG and SMC dependency tests.
  No check, debug path, environment switch or stress run remains.

- Immediate 32/64-bit SHLD/SHRD now bypass the dynamic count mask/select/guard graph. The frontend
  retains complementary immediate shifts for allocation, and a dedicated post-RA ARM64 builder
  fuses an adjacent single-use pair plus Or into one EXTR that writes the final allocated result.
  This placement is required: a first-class pre-RA funnel node changed fixed-home publication
  timing and caused deterministic SQLite heap corruption, so that design was removed. Parity token
  retention is restricted to the direct logical flags value proven to consume a fused result;
  the broad logical-token alternative grew SQLite by 592 instructions and was fully reverted. The
  exact SQLite common set keeps all 2,241 roots with 100% coverage and no growth, moves
  `264,582 -> 264,565`, and shrinks `powerOfTen@0x408ee0` `194 -> 177` versus FEX at 153. Adjusted
  output and the single-thread heap check pass. Smallpt remains 267 roots / 37,132 instructions with
  PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`;
  CoreMark 20k remains 299 roots / 37,160 instructions with `crcfinal=0x382f`. A six-form native x86
  differential confirms result plus defined CF/PF/ZF/SF byte-for-byte; its temporary check was
  deleted. Mac/Orb focused frontend, fusion, parity-token and logical-flags tests pass. No new env
  switch, diagnostic path or stress run remains.

- Full-width guest GPR copies can now transfer an old value version to the published target fixed
  home. The dedicated ARM64 builder proves the complete transparent-alias use set, target-home
  residency through the last use, and caller-saved helper preservation; it deliberately allows
  faulting memory uses because both architectural slots have coherent versions at every fault
  point. The first consumer covers only later memory addresses. In `sqlite3DefaultRowEst`,
  `mov x9, x1; mov x23, x9` becomes `mov x23, x1`, and the later loads use x23 after x1 is
  overwritten. The exact SQLite set keeps all 2,242 roots with 100% coverage and no growth, moving
  `264,573 -> 264,568`; five roots shrink by one instruction, including
  `sqlite3DefaultRowEst@0x40d240` `168 -> 167` versus FEX at 120. Timing-adjusted SQLite output is
  byte-identical. The bounded smallpt oracle keeps PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`; CoreMark stays at 299
  roots / 37,160 instructions with `crcfinal=0x382f`. Mac/Orb tests cover reuse across two faulting
  loads and invalidation by a target-home overwrite. No env switch, diagnostic path, fallback or
  stress run was added. The next consumer is pinned-GPR `KnownZeroAbove(16/32)` for the remaining
  compare/select width bridges in the same root.

- Narrow values published to pinned GPR homes now retain their proven zero-above width at exact
  post-publication consumers. The existing ARM64 builder enumerates every use of the narrow
  extension and records fixed-home mappings only for audited same-width U32 ALU/Select pairs;
  unrecognized uses or a target-home overwrite reject the whole recipe. `EmitSelect` consults that
  exact definition/consumer mapping and otherwise keeps the allocated register. In
  `sqlite3DefaultRowEst`, `ldrh w10; mov w13, w10; mov w22, w13` becomes `ldrh w22`, and the later
  shift plus `csel` read w22 directly. The exact SQLite set retains all 2,242 roots with 100%
  coverage and no growth, moving `264,568 -> 264,255` (`-313`, `-0.118306%`); 175 roots shrink and
  the target moves `167 -> 164` versus FEX at 120. Timing-adjusted output is byte-identical.
  Bounded smallpt retains PPM SHA-256
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`; its shared roots only
  shrink. CoreMark 20k retains `crcfinal=0x382f`, with all 299 baseline roots covered and only
  shrinkage. Mac/Orb pinned tests pass 97 assertions across 25 cases, including direct Select
  consumption and invalidation by a target-home overwrite. No environment switch, diagnostic
  path, fallback, check or stress run remains.

- Full-width values published to pinned GPR homes now expose an exact low-width view to later
  consumers. A dedicated ARM64 builder proves the complete producer use set, accepts only
  post-publication single-use zero-offset U8/U16/U32 extracts consumed by same-width Add, Sub or
  Select, and rejects target overwrites or caller-saved helper crossings. The publication itself
  remains on the existing SetHostGPR path. In `sqlite3DefaultRowEst@0x40d240`, three zero-shift
  extracts disappear and later Sub/Add/Sub operations read the published W view directly, moving
  the root `164 -> 161` versus FEX at 120. The exact SQLite set retains all 2,242 roots with 100%
  coverage and no growth, moving `264,255 -> 264,174`; 70 roots shrink and timing-adjusted output
  remains byte-identical. Bounded smallpt keeps all 267 roots, moves `37,128 -> 37,126`, and retains
  PPM SHA-256 `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`.
  CoreMark 20k keeps all 300 roots, moves `37,157 -> 37,156`, and retains `crcfinal=0x382f`.
  Mac/Orb focused tests cover the positive view and target-home overwrite rejection. No new env
  switch, diagnostic path, fallback, check or stress run remains.

- Zero-extended SelectZero results can now publish their low 32 bits directly to a pinned W home.
  A dedicated post-RA builder keeps the original IR/RA graph and proves the complete extension and
  low-alias use sets, the producer-to-publication fault/observer boundary, target-home lifetime and
  caller-saved helper preservation. When the proof holds, `EmitSelectZero` writes `CSEL` directly to
  the pinned W register and the extension/publication emit no instructions. Repeated SQLite
  candidates are shape-identical; after excluding two known baseline-optional variants above 20
  instructions, 32 stable common roots shrink by 99 instructions with no growth.
  `pcache1TruncateUnsafe@0x412f50` moves `188 -> 182` versus FEX at 141, and timing-adjusted output
  is byte-identical. The optional roots prevent an honest formal 99.9% join claim. Smallpt keeps all
  267 roots and moves `37,126 -> 37,125` with the same PPM SHA-256. CoreMark keeps 299 stable roots,
  moves the common set `37,146 -> 37,145`, and repeats `crcfinal=0x382f`. Mac/Orb pinned tests pass
  101 assertions across 27 cases, and Mac/Orb x86 div/idiv fuzz passes. Hot/cold tail deferral and
  generic zero-constant SSA deletion both caused reproducible SQLite heap corruption and were fully
  removed; the former requires an explicit serializable cold-stub contract and the latter changes
  RA/fixed-home lifetime. No check, env switch, debug path, fallback or stress run remains.

- The first versioned edge-flags ABI stage now carries a shared `EdgeFlagsState` and
  `EdgeFlagsTargetContract` through region proof, direct/static link sites, LinkManager/SMC state and
  disk-cache v15 records. A target that overwrites only `Flags::NZCV` before `AdvancePC` can accept a
  full-NZCV pending edge without the old false PF/AF requirement. Sources remain deliberately limited
  to the implemented full-NZCV state; partial target contracts do not publish dead pending/call entries.
  Fresh-HEAD smallpt `4 8 6` keeps all 279 roots with 100% coverage and the canonical PPM SHA, moving
  `49,590 -> 49,498` (`-92`, `-0.185521%`) with no growing root. Mac passes 1,107 non-stress
  direct-link, 46 region-flags, 31 NZCV and 11 indirect-fault-continuation assertions; Orb passes
  831, 46, 31 and 11. The Orb-only partial-NZCV false failure was an existing test iterator decoding
  three unaligned byte windows per ARM64 instruction; it now advances by VIXL instruction width, and
  the temporary disassembly capture was removed. No env switch, check, debug path or stress run remains.

- Contiguous partial-NZCV direct/static edges now reuse the same versioned contract and reversible
  merge patch as full NZCV. A target publishes a partial pending entry only when it overwrites every
  incoming valid bit before observation/fault and commits at `AdvancePC`; full sources cannot enter it,
  and pending-call L1 publication remains full-NZCV only. Target packed-flags versions now join exactly,
  moving the disk cache to v16. FlagM-canonical C sources declare Direct polarity; otherwise polarity
  stays Unknown. Mac passes 1,166 non-stress direct-link, 46 region-flags, 31 NZCV and 11 indirect-fault
  assertions; Orb passes 867, 46, 31 and 11. Same-commit smallpt static-only remains 279 roots and 49,498
  instructions at 100% coverage with the canonical PPM SHA. That is the expected static result because
  incompatible-target/SMC fallback bytes remain emitted; the linked partial path skips three executed
  merge instructions. SQLite and counter-based screens were stopped at the 8-second bound and are not
  cited as benefit evidence. No check, debug path, env switch or stress run remains.

- `888c3db` emits every function hot block before consuming explicit per-block `BlockColdPathRecipe`
  records for backedge, cycle, fault, VecNaN, density and flags-audit cold state. Standalone block
  translation consumes the same recipe immediately. A layout test proves the first block's NaN cold
  target follows the next hot block. `5dcaefb` separately initializes the indirect-L1 zero-key
  sentinel with the configured miss value, so guest target zero cannot key-hit a null host target.
  Mac passes 209 focused layout/continuation/fault/SMC/flags/NaN assertions, 840 production
  direct-link assertions, 29 continuation assertions and 793 non-stress SMC assertions. Fresh
  `63ad1de`/`888c3db` smallpt `4 8 6` static-only keeps all 279 roots, 49,498 instructions, 100%
  coverage and the canonical PPM SHA. SQLite `main/1` was stopped on both arms at the 8-second
  bound and is not benefit evidence. Orb SSH closed immediately during this stage, so the matching
  remote gate remains pending. No check, debug path, env switch, temporary source path or stress
  run remains. Next unify static/indirect/return/direct-link continuation publication and common
  cold tails before any trace scheduling.

- `0fb3113` adds a first-class ARM64 `ContinuationContract` for the `{x14 guest return, x30 host
  continuation}` frame and region traversal tags. Resolved static calls still enter the call entry;
  unresolved static calls publish the source continuation in the region trampoline before dispatcher
  traversal. Indirect-call key misses and invalidated target faults use per-site cold continuation
  preparation followed by a register-shared publisher; ordinary and pending-flags misses publish the
  frame before L1 fallback or flags/current-location recovery. Pre-call lookup guard faults retain the
  existing signal recovery and do not create a phantom call frame. A production test covers static and
  indirect `CodeMiss -> later target compilation -> guest return` and proves that x25 persists across
  the host round trip, returns to the original source continuation and becomes empty afterward. Mac
  passes 57 continuation, 785 production direct-link, 706 non-stress SMC, 33 indirect-L1, 5 guarded-RSB
  and 3 cold-layout assertions. Fresh `73082f5`/`0fb3113` smallpt `4 8 6` keeps 279 roots and the
  canonical PPM SHA; 17 call-miss cold roots add 50 instructions (`49,498 -> 49,548`) while 262 roots
  and the indirect-call hot-hit emitter remain unchanged. The single paired elapsed values
  `3.437s -> 3.464s` are consistency only. Orb SSH still closes immediately. No check, debug path,
  runtime env switch, temporary source path or compatibility fallback remains. The next continuation
  step is generation-aware unlink/invalidation; P0 external return entries can later remove the
  per-call-site cold continuation materialization.

- `340247d` makes the existing function-level multi-entry publication a first-class
  `FunctionEntryContract`. `FunctionDecodeFrontier` now exports stable accepted/rejected provenance
  with owner ranges, dependencies, call-return ownership and rejection reasons into HIR. Function
  compilation and disk restore publish canonical/direct/pending/call entries through one
  `FunctionEntryPublisher`, retaining the existing allocation owner and LinkManager generation/SMC
  transaction. Rejected ambiguous, call-return-owned, reset-failed or boundary-mismatched splits keep
  canonical L2 correctness but are not LinkManager targets; disk cache v17 preserves that unlinkable
  property across processes. Mac passes 42 function-entry, 355 JIT-cache and 42 region-production
  assertions. The bounded pre-stage/candidate smallpt `4 8 6` static pair is identical at 279 roots,
  49,548 instructions, 100% coverage and canonical PPM SHA, and the final candidate completes in
  3.415s. No long benchmark, stress run, check, env switch, diagnostic log, temporary source path or
  fallback remains. Next use the generation-aware external return entry to remove per-call-site
  call-miss continuation preparation, then remeasure the 50-instruction cold delta.

- `34ff5a9` replaces indirect call-miss exact site continuations with a fault-backed external
  continuation frame `{x14 guest return, 0 sentinel}`. Normal call hits still publish the exact BLR
  x30, and normal returns keep the same `LDP/CMP/B/BLR` hot sequence. Only the sentinel faults into a
  generation-aware L1/L2 return entry. Per-site miss/resume labels, `ADR x30,resume` and their shared
  branches are removed; guest-key mismatch still clears an untrusted RSB, while an external
  continuation fault consumes only the current frame and preserves outer frames. Disk cache v18
  carries the new recovery kind. Mac passes 105 continuation, 825 production direct-link, 742
  non-stress SMC, 33 indirect-L1, 5 guarded-stack and 63 serializer assertions. Bounded smallpt
  `4 8 6` keeps 279 roots and the canonical PPM SHA while moving `49,548 -> 49,520` (`-28`,
  `-0.056511%`). Broad terminal-only return publication was rejected after a short PageFatal and is
  absent from the tree; it needs an RA/live-in canonicalization proof. No long benchmark, stress run,
  check, env switch, diagnostic log, temporary source path or compatibility fallback remains.

- `4bf7117` centralizes direct/indirect helper ABI state in ARM64 `HelperCallContract`. Host-call
  capture emission and four pinned GPR builders now share one clobber model for preserve-all,
  FPCR, general-only, pinned-state and uniform effects. The first builder consumer permits x3-x9
  value versions to cross only `PreservesPinnedState` helpers backed by the resident string wrapper's
  explicit x3-x15/q16-q31 save set; x0-x2, unknown and indirect helpers remain barriers. Focused
  helper, pinned, XMM capture, AFP and CallLambda coverage passes 1,155 assertions. Smallpt remains
  279 roots / 49,520 instructions with the canonical PPM SHA. An 8-second SQLite pair has 714
  byte-identical common-root instruction counts but different truncated reachability, so no benefit
  is claimed. REP MOVS reports the same Unicorn/flags mismatch class with the old barrier and is not
  used as a gate. Fault, callback/reentry and host-NZCV effects stay conservative. No long benchmark,
  stress run, check, env switch, diagnostic log, temporary source path or fallback remains.

- `4b9e67e` adds canonical external CFG roots for shared direct targets inside an already decoded
  function block. The frontend records exact direct-jump sources without changing ordinary
  `SetLocation + ReturnToDispatch` IR. Targets with at least three sources still must have one exact
  owner, no call-return ownership and a successful boundary replay; accepted targets decode from a
  fresh frontend state and enter the function RPO as independent roots rather than inheriting HIR/RA
  live-ins. The split prefix uses `ExternalLinkBlock`; ARM64 commits dispatcher-visible state and
  branches to the target's published entry in the same allocation, retaining the fault-backed poll on
  backward edges and registering no direct-link site. Mac passes 66 function-entry, 860 production
  direct-link, 767 non-stress SMC and 105 continuation assertions. The exact Debug smallpt pair stays
  at 279 roots / 49,520 instructions with the canonical PPM SHA. SQLite's `freeSpace` binary has seven
  direct sources for `0x449b0c`, while `0x4498c9` and `0x449a5b` have one each, so only the shared exit
  meets the current profitability gate. The bounded Debug SQLite screen did not reach `freeSpace`;
  do not claim or broaden the optimization until the same commit receives a Release/Orb short shape.
  No stress run, check, env switch, diagnostic log, temporary source path or fallback remains.

- `a83b654` removes call-return ownership as a blanket external-root rejection when the split is
  before the call and the return block has one owner. Reset detaches that unique relation; canonical
  target replay reaches the call and registers the same return block again. Ambiguous and missing
  owners remain fail-closed. Release SQLite `main/size1` keeps 2,285 roots and moves total host
  instructions `370,399 -> 367,040`; `freeSpace` moves `482 -> 471`, with seven original direct
  sources plus one owner-prefix edge entering `0x449b0c`. Release smallpt keeps 279 roots and the
  canonical PPM SHA while moving `49,520 -> 49,265`. Mac passes 70 function-entry, 867 production
  direct-link, 773 non-stress SMC, 105 continuation and 38 extracted glibc assertions. Orb remains
  unverified. No stress run, long benchmark, check, env switch, diagnostic path or late-resolution
  fallback remains.

- `8e0927d` removes the emitter's contiguous-only pending-PSTATE gate. Any well-formed NZCV mask can
  now use the existing reversible direct/static-link bypass when the target contract overwrites every
  incoming bit before observation or fault and commits at `AdvancePC`; version checks and full-only
  pending-call publication are unchanged. The obsolete `HasContiguousPendingPState` predicate is
  deleted. Production coverage adds a non-contiguous `N|C` source/target, verifies the linked branch
  skips the complete variable-length merge, and restores the original merge on target invalidation.
  Mac passes 165 direct-link flags, 821 production direct-link, 42 production region-edge and 704
  non-stress SMC assertions. Release A/B stays identical at 279 roots / 49,265 instructions and the
  canonical PPM SHA for smallpt, and 2,187 roots / 356,265 instructions for SQLite main/size1. No
  stress run, long benchmark, check, env switch, diagnostic path or fallback remains. Next replace
  the separate region-successor proof with the shared target contract, then handle inverted/mixed
  carry joins.

- `d24e946` adds an explicit `RegionFlagsJoinRecipe` in a separate ARM64 join module. For a full-NZCV
  mixed successor pair, it moves the existing merge after the condition only when the canonical arm
  is the layout fallthrough, the compatible arm is cycle-free, and any live PF/AF token is killed by
  that compatible target. The hot compatible edge then enters before the merge; the canonical arm
  uses the registered token-aware shared merge trampoline. Every layout that would add a static
  branch keeps the old single pre-branch merge. The external target contract now records a four-bit
  observed NZCV mask separately from fault/helper barriers, and disk cache v19 preserves it. The
  same-allocation region proof remains capture-aware rather than inheriting the stricter external
  fault boundary. Mac passes 48 region-flags, 42 production region-edge, 167 direct-link flags, 794
  production direct-link, 302 JIT-cache, 682 non-stress SMC and 105 continuation assertions. Release
  A/B is exact at 279 roots / 49,265 instructions plus canonical PPM for smallpt and 2,187 roots /
  356,265 instructions for SQLite main/size1. A +878 strict-region regression and an unregistered
  outline self-loop were removed during screening. No stress run, long benchmark, check, env switch,
  diagnostic path or fallback remains. Next establish non-FlagM Direct/Inverted/Unknown source
  provenance before attempting non-fallthrough multi-predecessor joins.

- `0caec7c` makes non-FlagM edge carry polarity explicit. A per-block `EdgeCarrySourceState` observes
  only the existing U8 `ThreadContext64::carry_inverted` publication: constant 0/1 resolves to
  Direct/Inverted, while dynamic, wrong-width and non-boolean values remain Unknown. FlagM sources
  remain Direct and masks without C remain Unknown. This is compile-time metadata only; it emits no
  host instruction or runtime branch. The production static-forward matrix covers full Direct, NZ
  Unknown, non-contiguous `N|C` Direct and non-FlagM `N|C` Inverted through link and invalidation.
  Mac passes 200 direct-link flags, 97 focused static-forward, 913 production direct-link, 360
  JIT-cache, 48 region-flags, 769 non-stress SMC and 105 continuation assertions. Debug smallpt stays
  at 279 roots / 49,265 instructions with the canonical PPM; the 8-second Debug SQLite screen is not
  benefit evidence. No new env switch, check, diagnostic path or fallback remains. Next require a
  shared `{mask, polarity, version}` veneer before extending mixed joins beyond canonical fallthrough.

- `81405ec` introduces a block-local ARM64 `GuestStateMap` as the first capture-aware guest-state
  layer. It centralizes fixed-home survival, helper clobber and fault/observation window queries for
  full-width transfer, publication-view and SelectZero builders; the old per-builder scans are
  removed, and the existing `MayFaultOrObserve` users delegate to the same classification. A
  zero-offset U8/U16/U32 publication view may now serve multiple audited same-width Add/Sub/Select
  consumers when every ordinary use is accounted for and the fixed home survives to the last use.
  Mac Debug passes the 3-assertion multi-use case, 5 fault/overwrite assertions, 104 pinned and 37
  published assertions. Exact Release A/B remains identical at 279 roots / 49,265 instructions plus
  canonical PPM for smallpt and 2,188 roots / 356,545 instructions for SQLite main/size1. This stage
  does not claim benchmark shrinkage or the complete section-7 dataflow: guest-slot versions, width
  facts, fault captures, CFG joins and memory/XMM consumers remain. No stress run, long benchmark,
  env switch, check, diagnostic path, temporary source path or fallback remains.

- `6871978` adds a non-fallthrough canonical tail to mixed region flags joins. A full-NZCV source
  with exactly one compatible successor can branch its canonical arm to a cold stub when neither
  edge is a cycle/cut and the canonical arm is not the layout fallthrough. Stubs are shared by
  `{target, mask, polarity, version, token}` and contain only `ADR x17,target` plus a branch to the
  existing region merge trampoline; the merge body is not duplicated in the code object, and live
  PF/AF tokens use the existing x12 contract. A compatible-fallthrough production case proves the
  same guest flags, selector and halt result as FLAGS=0, places the branch before canonicalization,
  and has the same total code size as the existing canonical-fallthrough split. Mac Debug passes 60
  region-flags, 929 production direct-link, 105 continuation and 788 non-stress SMC assertions.
  Release smallpt remains 279 roots / 49,265 instructions with canonical PPM, and SQLite main/size1
  remains 2,188 roots / 356,545 instructions. Neither short workload hits the new tail, so no macro
  shrinkage is claimed. Partial-mask tails still require an exact-mask merge trampoline; no stress
  run, long benchmark, env switch, check, diagnostic path, temporary source path or fallback remains.

- `79bce76` closes the stateless terminal-only return-entry gap without publishing arbitrary empty
  blocks. `FunctionEntryContract::AnalyzeCanonicalTerminalEntries` seeds function roots, accepted
  external roots and uniquely owned call-return targets, accepts only empty constant LinkBlock/
  LinkBlockFast terminals, and propagates through a chain only when every predecessor is already
  canonical. These connectors are L2-only (`linkable=false`), use a adjusted one-byte guest range
  for disk-cache/SMC ownership, and branch to the next block's published label rather than inheriting
  its region-internal RA/live-in state. A discarded generic-external-edge version removed the old
  PageFatal but grew Debug smallpt by 186 instructions; none of it remains. Mac passes 87
  function-entry, 117 continuation, 829 production direct-link, 684 non-stress SMC and 309 JIT-cache
  assertions. Exact Release smallpt moves 279 roots / 49,265 instructions to 275 / 49,249: four
  four-instruction connector roots disappear and all 275 common roots are unchanged, with canonical
  PPM. SQLite main/size1 moves 2,188 / 356,545 to 2,115 / 356,245: 73 absorbed connectors account for
  290 instructions and the 2,115 common roots lose another 10 with no growth. SSA/PSTATE-bearing
  terminals remain rejected and require independent canonical replay. No stress run, long benchmark,
  env switch, check, diagnostic path, temporary source path or compatibility fallback remains.

- `0ad7d65` completes the static helper observation axes in `HelperCallTraits` and the ARM64
  `HelperCallContract`: implicit guest-state read/write, direct fault, dispatcher reentry and host
  NZCV preservation now default conservative and compose with the existing register, FPCR, ABI and
  uniform effects. `EmitHostCall` retains pending NZCV only when the complete contract proves there
  is no observable boundary and the wrapper preserves host flags. GuestStateMap and region scans are
  instruction-aware, while physical NZCV and x12 token clobbers remain separate proofs. The first
  production consumer is the resident REP-string wrapper, which carries NZCV through the AAPCS64
  callee-saved low half of d8; d8 remains in the wrapper's declared caller-clobbered FPR set. A real
  `SwiftRepStos1Resident` execution case matches the no-helper pending-flags baseline. Mac Debug
  passes 86 helper, 60 region-flags and 5 pinned-value assertions. Exact Release static A/B keeps
  smallpt at 275 roots / 49,249 instructions with canonical PPM; SQLite matches all 2,114 roots and
  top-20 while moving `355,965 -> 355,961` (`-4`, two roots shrink by two, none grow). Unknown and
  indirect helpers stay fail-closed. No stress run, long benchmark, env switch, check, diagnostic
  path, temporary source/build path or compatibility fallback remains. Next extend GuestStateMap
  from block-local fixed homes to guest-slot versions, width facts, fault captures and safe CFG joins.

- `2d2556e` adds block-local pinned guest value versions to `GuestStateMap`. Each resident value now
  carries its fixed home, W/X width and physical high-32 normalization fact. Entry reads, zero-offset
  publications and `ZeroExtend32To64` low views share one active-state model; helper clobbers,
  later publications and the earlier physical write point of RA-coalesced stores invalidate the
  correct version. Consumer-specific transferred uses moved out of the translator's former
  `pinned_gpr_use_homes` table and into the same map. Proven `SetHostGPR`, `Add`, `Select` and U32
  `Sub/And/Or/Xor` users read the resident home directly, and a fully resident `GetHostGPR` emits no
  move. Mac Debug passes 104 pinned, 86 helper and 60 region-flags assertions. Exact Release A/B
  keeps 275 smallpt roots with canonical PPM and moves `49,249 -> 49,107` (`-142`); SQLite matches
  all 2,114 roots/top-20 and moves `355,961 -> 354,915` (`-1,046`) with no growth and rc=0. A single
  local profile shows about 34 ms additional codegen time across 2,114 functions, so it is recorded
  as an analysis cost rather than hidden by noisy TOTAL timing. No stress run, long benchmark, env
  switch, check, diagnostic path, temporary source/build path or fallback remains. The next mechanism
  boundary is fault-visible captures and safe diamond/backedge joins; this commit does not claim
  cross-CFG facts.

- `4fd6d24` adds demand-driven fault-visible width facts and CFG joins to `GuestStateMap`. Function,
  external and call-return roots start Unknown; internal diamonds and backedges use a fixed-point
  predecessor meet. U32 publications establish physical high-zero state, while full/partial writes
  and opaque calls transfer or invalidate it conservatively. Fault boundaries capture the currently
  published `{version, home, width}` state before the faulting instruction, and an early pinned-home
  publication is legal only when every intervening capture already contains that version. CFG
  solving runs only for a block with an actual entry-width or SelectZero capture consumer; an eager
  prototype added about 27 ms across SQLite's 2,114 functions and was removed. Mac Debug passes 110
  pinned, 56 fault-capture, 4 SelectZero, 86 helper and 60 region-flags assertions, including
  external-root, diamond, backedge and real PageFatal W-high-zero cases. Exact Release A/B is neutral:
  smallpt remains 275 roots / 49,107 instructions with canonical PPM, and SQLite remains 2,114 roots /
  354,915 instructions with full root/top-20 coverage and rc=0. No stress run, long benchmark, env
  switch, check, diagnostic path, temporary source/build path or fallback remains. Next add 8/16-bit
  zero/sign-extension facts and let memory/compare consumers use the joined lattice.

- `f4ca978` replaces the old high-32 boolean with one `ExtensionFacts` lattice carrying
  `KnownZeroAbove(8/16/32)` and bounded sign-extension ranges. Publications preserve nested
  ZeroExtend/SignExtend aliases; partial writes, helper clobbers, external roots, diamonds and
  backedges transfer or meet the same facts. Repeated U8/U16 zero/sign extensions may consume the
  resident home only when its version and full extension range still match. Extension consumers do
  not yet participate in definition-level GetHost elimination: an intermediate version combined
  that with the older fused-zext early return and left a result register uncomputed, causing both
  short workloads to stop after 84 roots; the rejected path is fully removed. Mac Debug passes 120
  pinned, 56 fault-capture, 4 SelectZero, 86 helper and 60 region-flags assertions. Exact Release A/B
  against `911c57e` keeps all roots/top-20 with no growth: smallpt `49,107 -> 49,095` (`-12`) plus
  canonical PPM, and SQLite `354,915 -> 354,800` (`-115`) with rc=0. One profile pair is effectively
  flat in TOTAL (`1.486s -> 1.485s`); no long run is used as evidence. No env switch, check, log,
  temporary source/build path or fallback remains. Next route narrow memory/compare and XMM scalar
  consumers through this lattice instead of adding more producer matchers.

- `b69d594` routes narrow compare and ordinary memory-store operands through the same resident
  version lattice. U8/U16 compare requires an exact version plus `KnownZeroAbove(width)` before it
  can read the W home; narrow StoreMemory requires exact version/home/width and otherwise falls
  through to the existing canonical allocator path. Focused codegen emits `cmp w22,#5` and
  `strb w22` directly. Mac Debug passes the 2 consumer assertions plus 120 pinned, 56 fault-capture,
  4 SelectZero, 86 helper and 60 region-flags assertions. Exact Release A/B against `f4ca978` keeps
  every root/top-20 with no growth: smallpt `49,095 -> 49,065` (`-30`) plus canonical PPM, and SQLite
  `354,800 -> 354,519` (`-281`) with rc=0. Candidate codegen is 315.6 ms and TOTAL 1.483 s in one
  consistency profile. No stress run, long benchmark, env switch, check, log, temporary path or
  fallback remains. The section-7 GPR lattice now has ALU-extension, compare and memory consumers;
  audit XMM scalar reuse before broadening it further.

- `3de2a5a` binds persisted host continuations to the existing QSBR reclaim generation. Each
  `RuntimeEpoch` records the generation consumed by its RSB state, and `BeginJit` clears an obsolete
  stack before any cache lookup; the multithreaded path reuses its existing `global_epoch_` load and
  generated call/return code is unchanged. A recognized same-thread SMC fault also resets x25 in the
  signal context through the guarded return-stack owner, while active cross-thread code remains
  protected until its QSBR exit. The production allocation-retirement lifecycle passes 144 focused
  assertions; continuation, production direct-link, non-stress SMC, indirect-L1 and guarded-RSB
  groups pass. Release static shape remains smallpt `275 / 49,065` and SQLite `2,114 / 354,519`.
  A temporary exact-HEAD SQLite pair is `TOTAL 1.555s -> 1.554s`; all temporary worktrees, builds and
  captures were removed. No frame generation field, hot return instruction, env switch, check, log,
  temporary path or fallback remains. Continuation generation/unlink/invalidation and code-cache
  reuse are now closed; next audit XMM scalar GuestStateMap reuse and the remaining exact-mask
  EdgeFlags tail before entering string/complex-EA work.

- `ee04fb9` closes the real partial-mask region-flags tail. A function stub now carries only the
  canonical target and branches to one of 14 per-region exact-mask merge entries; contiguous and
  non-contiguous masks share the same contract, and separate token entries preserve packed PF/AF.
  The first dynamic-mask design grew SQLite by 14 instructions and was removed. The retained
  per-mask design keeps 100% root/top-20 coverage with no growing root: smallpt moves
  `49,065 -> 49,055` (`-10`) and SQLite `354,519 -> 354,491` (`-28`, 19 shrinking roots). Masked
  trampoline execution covers `N|Z`, `N|C`, token and non-token forms; region-flags production,
  trampoline, production direct-link, direct-link flags and non-stress SMC groups pass. A single
  SQLite consistency pair is `TOTAL 1.571s -> 1.573s`. The narrow XMM same-value self-publication
  version candidate was byte-identical on both workloads and was deleted. No check, env switch,
  log, temporary path or fallback remains. Do not fake a nonzero `packed_flags_version`; remove that
  unused speculative ABI next, then re-rank string/complex-EA roots.

- `d7076b7` removes the unused `packed_flags_version` state and target-contract field instead of
  manufacturing a nonzero layout. Link-site/target records shrink `80/96 -> 72/88` bytes; disk-cache
  block and edge records drop both serialized u64 slots and the format advances `v19 -> v20`, with
  no legacy reader. Direct-link flags, serializer, full JIT-cache, region-flags production and
  trampoline groups pass. Release shape stays smallpt `275 / 49,055` and SQLite
  `2,114 / 354,491`. EdgeFlags now has no unimplemented state dimension; next work should be chosen
  from measured string/complex-EA residuals rather than another flags representation layer.

- `51c783c` compacts inline SSE4.2 result packing: the already bounded IntRes2 no longer pays a
  final mask, CF reuses the zero test made before index packing, and most-significant index uses
  `CLZ+EOR` instead of `CLZ+MOV+SUB`. The Rosetta/SDM differential passes 16,255 assertions, with
  memory-boundary and alias/REX groups also green. Strict SQLite keeps 2,114 roots and moves
  `354,491 -> 354,486` (`-5`) solely in `__strspn_sse42` (`318 -> 313`), with no growth; smallpt
  stays `275 / 49,055`. A single SQLite pair is `TOTAL 1.485s -> 1.490s` while translation/codegen
  both decrease slightly. The shared `0x02/0x1a` helper boundary is unchanged, so do not claim a
  `strcspn`/`strcmp` win or retry per-unit EqualAny outlining. No check, log, env switch, temporary
  path or fallback remains.

- `3b42c72` completes miss-driven canonical region membership. A first 64-block region records only
  its still-unpublished external roots and strong owner direct; a later real code miss consumes the
  record, retires the exact old allocation, recompiles the old canonical region first and appends only
  blocks not already claimed by it. The 128-block ceiling remains explicit, and the builder, decoder,
  SMC retirement and backend code-object emitter stay in separate modules. Retirement clears every
  allocation-owned alias from shared and private dispatch tables, restores incoming direct links and
  uses the existing QSBR path. Private-L1 invalidation now faults through the slot-corresponding 4 MiB
  guard mapping, reconstructs the guest key and rebuilds the L1 slot address before L2 fallback.
  Empty external placeholders can transfer to the primary function without accepting overlapping real
  definitions. A rejected new-root-first ordering produced `248 roots / 248 versions / 41,221`
  instructions (`+4,773` over the 16.73 static baseline). The retained old-root-first ordering is
  stable across two short smallpt runs at `243 roots / 249 versions / 36,427` instructions versus
  `249 / 249 / 36,448`, with the canonical PPM SHA unchanged. Mac and Orb membership,
  function-code-object, function-entry, continuation, indirect-L1, non-stress direct-link and
  non-stress SMC groups pass. No long benchmark, stress run, check, env switch, debug log, temporary
  source path or fallback remains. Next run the short FEX-aligned root comparison before deciding
  between a third canonical region and the larger remaining hot/cold-layout or cross-root state-ABI
  mechanism.

- `c5984d9` fixes the default-regroup SQLite crash and restores continuation safety. External
  call-miss frames publish `{guest_key, 0}` by design, so a ret whose predicted key still matches
  reached `blr` on a zero host continuation; the `1dff969` deferred-fault model only recovers that
  site while the *popping* object's fault metadata is still registered, and the regroup path can
  retire the object holding the frame's referent first. The pop now checks `cbz` after the
  predicted-key match and falls back to the shared L1 dispatch — not the mismatch path, which would
  reset the whole stack and drop live lower frames. Ordering matters: the `cbz` must follow the key
  compare, or a popped garbage frame on a reset stack skips the reset and leaves `x25` above empty.
  Focused continuation/direct-link/indirect-L1/call-miss/fault/SMC groups pass on Mac and Orb
  (118 + 901k + 52 + 66 + 36 + 452 assertions), and three clean default SQLite `main/10` runs
  report `TOTAL ~2.0s`.

- Fresh FEX-aligned joins were rebuilt against the live `f2e35f3` measurement build with a new
  retained tool `tools/svm-linux-cq/fex_join.py`. Two metrics are now printed; read both before
  quoting a number:

  - **Block-volume join** (every SVM hot PC joins the tightest containing FEX
    `[rip, rip+guest_bytes)` block; per block compare `Σ svm.host_static` vs `fex.host_inst`,
    weighted by covered entries): SQLite 0.840×, CoreMark 0.954×, smallpt 0.863×, c-ray 0.784×,
    OpenSSL-SHA256 0.645× — SVM emits less total host code per covered window. CAVEAT: SVM's lazy
    decode covers fewer guest instructions inside a window than FEX's eager multiblock, so this
    ratio flatters SVM and is NOT the density metric.
  - **Per-PC density join** (documented formula — per SVM block-PC compare `host_static` vs its
    decoded guest-instruction count against the containing FEX block's `host_inst/guest_inst`):
    - entries-weighted (dynamic): SQLite **1.00×** parity, CoreMark **1.18×**, smallpt **1.22×**,
      c-ray **0.90×** ahead.
    - guest_inst-weighted (static): SQLite **1.01×**, CoreMark **1.06×**, smallpt **1.08×**.
    Either weighting: SVM is at parity-to-slightly-behind on work-density, not ahead.

  Guest-instruction counts for SVM blocks come from `[svm-gap-block] ... bytes=N insts=N` lines
  under `SVM_DENSITY_PROF=1 SVM_RA_HOT_COALESCE_ALL=1` — `bytes` is the authoritative decoded
  span (`GetEndLocation()-GetStartLocation()`); `insts` (AdvancePC count) undercounts ~2× because
  IR passes eliminate/merge AdvancePC ops — the join counts real instructions by disassembling
  `[block, block+bytes)` with llvm-objdump instead. FEX `guest_inst` is their own decode count
  (multiblock ranges include undecoded gap code, so never count insts inside a FEX span).

  sqlite RE=0 (`SVM_REGION_EDGES=0`) measures 1.24× behind vs default-region 1.00× — the region
  pipeline is already denser than block-per-entry for this workload. Correctness
  gates held during capture: SQLite stdout complete, smallpt PPM md5 `5a34cbe0…`, CoreMark
  `crcfinal=0xd340`. The largest remaining weighted gaps are all SVM-multi-root duplication inside a
  single small FEX block (`0x4aca2f`: 87 units / 684 host vs FEX 51; `0x46febd`: 245 units /
  1,479 vs 107; `0x425aad`: 260 units / 2,094 vs 403). Per the corrected density readout, the
  next target is per-instruction overhead in hot units — the block-volume rows mix in
  gap-attributed SVM code and overstate the per-site excess.

- `smc_mt_stress` retains a ~1% host-fail flake (`SIGSEGV` from JIT code storing to a
  guest-mapped-but-inaccessible page near a guard boundary) that fires identically with
  `SVM_FUNC_LAZY=63` — it is a pre-existing hazard in the SMC invalidation path, not introduced by
  regroup or `c5984d9`. `run_helper_fault_tests.sh` `fxrstor` fails on a pre-existing
  scratch-GPR-budget assert (`declared 0, asked for 1`), unrelated. `run_dynamic_tests.sh` marks
  its three glibc cases FAIL on Orb only because the direct-mode notice line joins stdout; the
  guest output and exit 42 are correct.

- **Eager function formation (`SVM_FUNC_LAZY=0`, lazy=false) closes the multi-root duplication
  gap and beats FEX on the static per-PC density metric on every measured workload**: CoreMark
  ~0.84×, smallpt ~0.83×, SQLite ~0.89× (all ahead; was 1.06/1.08/1.01 behind-or-parity under the
  default lazy region). The win is not a larger window — it is that each entry's reachable body is
  decoded into ONE object, so internal loop entries (e.g. CoreMark `0x402683`) fall through inside
  the block instead of becoming a separate member block with its own boundary/prologue. Lazy
  regions cannot cheaply reproduce this: candidates are created per discovered edge target, and
  `NearestEntry` splits at each one regardless of `has_code`/`local_target` — confirmed by removing
  the `has_code` skip (no density change) and by inspecting decode order. A lazy-side match needs
  IR-level mid-entry labels, a larger refactor.

  Wall-clock on completing runs also favors eager (`--version` 64ms vs 140ms; sqlite `main/10`
  guest TOTAL ~1.0s vs ~2.2s; smallpt/coremark also faster). Eager emits fewer total decoded
  blocks (CoreMark 1,551 vs lazy 3,735) — lazy re-decodes overlapping member blocks across objects.

  **Blocker — do not make eager the default yet.** `SVM_FUNC_LAZY=0` on sqlite `main/40`
  deterministically dies during test 100 (`20000 INSERTs`): host `SIGSEGV` on a wild guest
  pointer plus `free(): invalid next size` heap corruption. It is avoided by `SVM_BLOCK_LINK=0`
  (direct-link off → completes, 6.48s) and by `SVM_REGION_EDGES=0` (different codegen → completes
  but loses the fusion win). Exec-trace shows recovered fetch-faults where `host_pc==fault_addr`
  on 0xfffe… addresses = jumps into reclaimed code, so this is the known stale direct-link /
  code-reclamation hazard class that eager's bigger-object lifecycle exposes. Original code masked
  it because the first `kMaxFuncBlocks` overflow latched `function_compilation_disabled` and every
  later function went block-only. Root cause is inside the link/reclaim lifecycle
  (`DelinkTargets`/`ClearDispatchSlots` already walk `Function::GetBlocks()`, so the hole is not
  the direct dispatch tables — the likelier uncovered caches are the per-thread `indirect_l1`
  table and the **RSB/continuation frames**, which hold raw host addresses of mid-object
  call-return points and are only resynced at `BeginJit` epoch boundaries, leaving live frames
  stale across an in-flight reclaim), not the cap-retry fix.

  Supporting fix landed: `[svm-gap-block]` now sums `AdvancePC` immediates for the guest span
  (the old `end-start` read zero pre-finalization and fabricated invalid spans at larger
  budgets) — `2d10da9`. `FUNC_LAZY=128` stays ambiguous (heap-corruption flake under the density
  profiler); `FUNC_LAZY>=129` and `=0` are equivalent eager.

  RESOLVED (`95c1103`): the size-40 eager fault was an object-ownership overlap, not a
  reclamation race. The region decoder's `has_code` boundary skip was gated on `config.lazy`, so
  an eager decode could re-absorb a block already published by an earlier object — orphaning it
  and leaving inbound direct/indirect links pointing at dead code. The guard now applies in both
  modes (under eager `IsAccepted` is never set, so it degrades to the basic `has_code` check), and
  an oversized eager function retries once through the bounded lazy-region path instead of
  latching `function_compilation_disabled` (the latch only re-arms if the region also overflows).
  sqlite `main/40` now completes cleanly under `SVM_FUNC_LAZY=0` (15+/15, integrity pass) where it
  previously SIGSEGV'd on a dead code page. Earlier observation that "the latch is load-bearing"
  was masking this overlap — the basic per-function `block_only` fallback still overlapped; only
  the has_code boundary + region retry removes it. Note c-ray currently halts on an unrelated
  pre-existing guest fixed-map failure (`errno 17` at 0xfffe08000000) in BOTH modes — not a
  codegen regression.

### Density recalibration after the eager fix (2026-09-11)

Re-profiled eager CoreMark on the post-`95c1103` build to pick the next optimization target.
Key correction to the earlier "move ops dominate" framing:

- **`move`-class IR ops are ~50% of op *count* but mostly emit 0 host bytes** — `GetHostGPR`,
  `SetHostGPR`, `GetOperand`, `GetResult`, `DefineLocal`, most `LoadImm`/`ZeroExtend` are already
  coalesced away by the register allocator + the many `EmitGetHostGPR`/`EmitSetHostGPR` fusion
  shapes. Op-count share is misleading; **emitted bytes is the honest axis**.
- Per-op emitted bytes (eager coremark, `[svm-gap-op]`): `LoadMemory` 10.3%, `StoreMemory` 9.4%,
  `GetOperand` 9.6%, `LoadImm` 9.1%, `SetHostGPR` residual 7.7%, `SetLocation` 7.6% — i.e. real
  memory work + effective-address materialization + guest-PC stores. `GetHostGPR` is only ~1.5%
  despite high op count.
- **Instrumentation caveat**: `SVM_RA_HOT_COALESCE_ALL=1` injects a ~36–40B (9-inst)
  `RecordHotCounter` check at every block entry — `[svm-boundary]` `bytes_prologue` reads that
  check and looks like a per-block prologue (≈160KB total). With `SVM_DENSITY_PROF` alone
  `bytes_prologue=0` for every block — **there is no per-block prologue**; pinned GPRs/state are
  persistent, not reloaded. The clean `[svm-density]` split (checks excluded) is
  work 41.5% / boundary 27.9% / move 24.1% / flags 4.8% / uniform 1.7%. Boundary here is all
  exits+links (dispatch machinery), no entry reload. `host_instructions` in `[svm-hot-all]`
  already subtracts checks, so the equiv-metric numbers were always clean.
- **Concrete gap vs FEX = unit granularity.** FEX `0x4024c0` = 1 block, 124 host insts for 86
  guest insts. SVM splits the same region into **4 units** (`0x4024c0`, `0x40251b`, `0x40254f`,
  `0x402600`). `0x40251b`/`0x40254f` are *internal* branch targets (`jmp8` from `0x40250a` /
  `0x402528`) that became separate roots because they were reached via an external/indirect
  entry before the enclosing function decoded — once published, the `has_code` boundary
  (correctly) keeps them from being absorbed. Each fragment re-pays the exit+link overhead.
- **Honest-metric status (equiv host/guest-inst)**: SVM is ahead of FEX on every cleanly
  measurable workload. The per-PC `gap members` join stays misleading for fused eager units
  (no-member-pcs) — keep using the equiv/weighted numbers.

  Verified equiv `SVM/FEX` host-instruction ratios (`FEX_BLOCKSTATS=1 FEX_MULTIBLOCK=1
  FEX_HOSTFEATURES=disableavx`, `/usr/local/fex-measure/FEX` @ `f2e35f3`, joined with
  `tools/svm-linux-cq/fex_join.py`; SVM numbers are check-clean — `[svm-hot-all]` already
  subtracts hot-counter instrumentation):

  | workload | equiv SVM/FEX | weighted svm_host | weighted fex_host | entry cov |
  |---|---|---|---|---|
  | ossl sha256 | **0.615** | 371.77e9 | 604.30e9 | 98.8% |
  | ossl aes-128-gcm | **0.709** | 0.541e9 | 0.764e9 | 99.4% |
  | sqlite `main` | **0.876** | — | — | — |
  | smallpt | **0.877** | 25.73e9 | 29.33e9 | 93.5% |
  | coremark | **0.954** | 13.95e9 | 14.62e9 | ~100% |

  Coremark is **identical under lazy and eager** (both `≈0.954`) — the density win is not
  eager-specific; eager mainly saves wall-clock recompile, not code density.

**Next real lever (structural, not instruction-level):** the per-unit entry+exit is amortized
by *larger* units. To close the residual gap, either (a) multi-entry units — let a
mid-function pc reached via dispatch enter the enclosing unit at an internal label instead of
compiling a fragment (the doc's known "IR-level mid-entry labels" refactor; blocked by SSA
live-ins needing state re-derivation on a fresh entry), or (b) reduce the fixed entry cost
(pinned-GPR reload elision for regs the unit provably doesn't read). Both carry correctness
risk — the eager bug was exactly this ownership/lifecycle class.

**Coverage blockers (correctness, not density):** the remaining gap to "every workload" is
crashes, not code quality.

- **7-Zip — SVM-side guest-register corruption (all modes).** Deterministic guest fault:
  `movzx eax, byte[rbp+rcx]` at `0x62a527` (LZMA match-finder byte-table loop). At the halt
  `rcx=0x10ce24` is **even**, but the loop counter only ever takes `1,3,5,…` (`mov $1,%ecx` /
  `add $2,%rcx`) — an even rcx is *impossible* via the loop's own increment, so a guest GPR
  was bound to the wrong value (an RA/SSA corruption, not a bad loop bound — `rdx` correctly
  holds `0xb`). Reproduces under eager (`FUNC_LAZY=0`) and lazy-region (`FUNC_LAZY=1/64/128`),
  and independent of `SVM_RA_COALESCE`/`SVM_RA_SPILL_EVICT`/`SVM_UNIFORM_*` (each just moves
  the crash). Secondary `free(): invalid pointer` / `malloc_consolidate` / `double free`
  signatures appear on some toggles — a heap-corruption symptom worth an ASan/valgrind pass.
  Added a full guest-GPR dump to the `Guest halted` path (`translator/linux/main.cpp`) for
  this class of bug.
- **c-ray** — pre-existing guest crash near `0xfffe08000000`, both modes (not a regression).
- **stream** — hangs under default `SVM_MEM_DIRECT=1`; exits under `=0` (verify output).

## Orb loop

```
M=/mnt/mac/Users/swift/CLionProjects/SwiftVM
P=/home/swift/svm-phasec/SwiftVM
git ls-files -z | rsync -a --no-times --checksum --from0 --files-from=- ./ ubuntu@orb:$P/
cmake --build /home/swift/svm-phasec/build --target svm_translator_linux -j2
```

Keep `--no-times --checksum` for A/B source switches. Preserved older source mtimes can otherwise
leave a newer Ninja object in place even though the source contents changed.
Keep the default build parallelism at two jobs; raise it only for an explicitly requested one-off
build. Short benchmark gates must not be replaced with stress or long-run loops.

Mac: `cmake --build build-master --target swift_runtime`.

func_tests one-iter coremark can halt reason 2 even on good binaries; use **20000** iters for CRC.

## Related docs (historical, some defaults stale)

- `docs/fex-codegen-gap-recipe-2026-08.md` — combo: flags × region × RA
- `docs/codegen-p0b-flags-repr-2026-08.md` — still says FLAGS default OFF / 16-block in places
- `docs/svm-config-classification.md`
