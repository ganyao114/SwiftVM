# Exception recovery implementation evidence

- [Implementation and limitations](../../exception-context-recovery-implementation-2026-11-09.md).
- [Fixed final measurement plan](measurement-plan.json), [input identities](inputs.json), [performance summary](performance-summary.json).
- Every final observation: [120 compile records](compile-records.json), [30 runtime records](runtime-records.json). All fixed3 samples retained; no minimum selection. Initial frozen implementation remains separately in Data2/guard, final version in Data2/guard-final.
- [PMU plan](instruction-clock-plan.json), [records](instruction-clock-records.json), [summary](instruction-clock-summary.json). Raw timer fields are **retired user instructions**, not nanoseconds. Six Function observations match formal work counts; all six Block mismatches remain visible and do not enter matched ratios.
- [Thread CPU plan](thread-clock-plan.json), [records](thread-clock-records.json), [summary](thread-clock-summary.json). Only exact steady_clock callers intercepted; guest clocks remain real. Identical syscall wrapper overhead, diagnostic only. Six Function observations match; Block mismatches excluded from matched ratios. The first marker-only harness failure is retained separately in Data2.
- [Host10 contracts](host-contracts-10.log), [final relevant2 contracts](host-contracts-final.log).
- Android: [exceptions/API](android-exceptions.json), [signals](android-signals.json), [runtime](android-runtime.json), [fault ABI](android-fault-abi.json), [persistence checkpoint](android-persistence.json). Persistence is explicitly an earlier checkpoint; no claim it used the final test binary.
- Cross-host: [48-case checkpoint](cross-checkpoint-summary.json), [21 targeted final repeats](cross-final-summary.json). Real Linux kernels under QEMU system, no hardware performance claim. Versions/hashes remain distinct; 69 executions are not 69 unique tests.
- [Source freeze](source-freeze.json), [source match](source-match.json), [format checks](format-check.json). Actual formatter output is byte-identical for123 eligible files; clang-format22 dry-run false positive on the function-try catch is preserved.

Full raw reports, all failures, scripts and binary archives:
`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/exception-recovery-20261109`.
Absolute JSON paths identify the original run and are deliberately not rewritten.

SVM includes complete native ownership release; the fixed FEX probe excludes final native arena/directory destruction. Compare FEX multiblock compilation against SVM Function, keep Block separate. CoreMark short20 is compile/CRC only; runtime validates100000 iterations. x87 fixture has exit-only validation. No new FEX runtime/PMU comparison was made.

- [Final candidate archive identity](candidate-identity.json), [phone cleanup](phone-cleanup-receipt.json). The decompressed parent copy is recycled after checking its preserved original gzip.

[Submission](submission.json) binds the final source manifest to the implementation commit. The repository host log copy trims its final empty line; the exact original log remains in Data2.
