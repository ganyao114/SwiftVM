# Compile shape and ownership checkpoint

Final runner: `ee0ae5c47f8988dd7d339fcbc0787e361aee27465316f4901dbbea52a0cdbac9`.
Parent runner and Git base are recorded in [parent.json](parent.json).

All final observations are retained: 72 compile, 72 runtime, 48 alternating
parent/candidate, 18 optional phase diagnostics and 24 instruction-clock diagnostics.
No observations from `pre-dedup`, `pre-metadata` or `pre-liveness-revert` enter the
final tables. One unqualified diagnostic attempt remains separately in the
original result root and is not included in the 24 qualified observations.

- [Compile/runtime summary](benchmark-summary.json), [compile records](compile-records.json),
  [runtime records](runtime-records.json), [exclusive stages](compile-breakdown.json).
- [Paired results](paired-summary.json), [all paired observations](paired-records.json).
- [Optional phase records](phase-records.json) use extra clocks and never enter formal totals.
- [Instruction-clock plan](instruction-clock-plan.json), [records](instruction-clock-records.json),
  [summary](instruction-clock-summary.json). **Every timer-number field in these raw
  reports is retired user instructions, not nanoseconds.** The measurement-only
  preload library substitutes a pinned per-thread PMU counter for CLOCK_MONOTONIC;
  every read requires enabled=running. It also substitutes host calls made on
  behalf of the guest. Checksums and the listed compilation work-count sets must
  match formal observations. These are diagnostic instruction counts, not elapsed
  time or FEX ratios, and include counter-wrapper overhead.
  The summary uses `compile_without_byte_decode_ns`: instruction counts excluding
  the byte parser, despite the inherited field name.
- [Source identities](source-freeze.json), [post-run match](product-source-match.json),
  [remote input identities](runner-identity-after.json), [format](format-check.json).
- [Host](host-tests.json), [Android](android-tests.json), [x64](x64-summary.json),
  [RV/LA kernel-03](rv-la-summary.json), [phone cleanup](phone-cleanup-receipt.json).

SVM includes complete native-owner release. FEX's existing logical work-buffer
release probe excludes final native arena/lookup destruction; these endpoints
remain different. FEX RA is grouped with allocation, despite running in PassManager.
CoreMark and func_tests work-count variations are retained; see the implementation
report for the exact cases. The x87 fixture has exit-only validation.

Full scripts, stdout/stderr, source archives, compressed runners, old checkpoints,
and kernel logs remain at:
`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/compile-shape-optimization-20261108`.
Absolute paths in the JSON identify the original run; they are not rewritten.
