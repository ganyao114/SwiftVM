# Dispatch demand checkpoint evidence

This is the retained text/JSON subset of the final 2026-11-08 RedMagic checkpoint.
It contains every normalized observation: 72 compile, 72 runtime, and 48 paired
parent/candidate runs. No observation is selected or replaced by a faster run.

- [Plan](plan.json), [input identities](inputs.json), [environment](environment.json).
- [Benchmark summary](benchmark-summary.json), [compile records](compile-records.json),
  [runtime records](runtime-records.json), [exclusive compile stages](compile-breakdown.json).
- [Paired plan](paired-plan.json), [all paired records](paired-records.json),
  [paired summary](paired-summary.json).
- [Measured source snapshot hashes](source-freeze.json),
  [unchanged product source check](product-source-match.json), [runner identity](candidate-identity.json).
- [Android tests](android-tests.json), [host tests](host-tests.json),
  [x64](x64-summary.json), [RV/LA](rv-la-summary.json).
- [Phone cleanup](phone-cleanup-receipt.json), [file identities](manifest.json).

Original absolute paths in JSON identify the original measurement location;
they have not been rewritten to imply a new measurement or a committed source
version. Product source and CMake match the measured snapshot. Submission adds
documentation links and updates lifecycle descriptions only.

The report and complete compile window include SVM native owner release. FEX's
existing logical-buffer release probe excludes final native arena and lookup
cache destruction. Preserve this endpoint difference when comparing stages.

Full stdout/stderr, scripts, source archive, compressed runner and kernel console
logs remain at `/Volumes/Data2/swiftvm-middle-results-relocated-20261025/dispatch-demand-20261108`.
The rejected FP cold-layout experiment is separately retained and is absent from
the submitted product. See [the implementation report](../../dispatch-resource-optimization-2026-11-08.md).
