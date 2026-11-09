# L1 dispatch interrupt evidence

Final implementation in the `SwiftVM-submit-20261104` worktree, based on `a5a2b93c`.

- `plan.json`, `inputs.json`: fixed final paired plan and exact deployed identities; no new FEX measurements.
- `hot-records.json`, `hot-summary.json`: all 48 final warmed CALL/JMP observations and medians.
- `compile-records.json`, `compile-summary.json`, `work-counts.json`: all 36 final compile observations, ranges and emitted work. Complete existing SVM compile ownership endpoint; subtract only byte decode.
- `android-*.json`: final RedMagic functional cases, including native lookup traps, asynchronous target-PC recovery, cancellation/empty drain, guest fault separation, signals, SMC/FP domains and persistence.
- `cross-summary.json`: x64/RV64/LA64 system-kernel baseline checks, 27 cases. `cross-drain-summary.json`: final empty-drain fix, 6 targeted cases. QEMU system, no hardware performance claims.
- `frontend-contracts.log`, `host-contracts-final.log`, `format-check.json`: frontend/direct, ARM64 emission/fallback, code image and formatting checks.
- `final-source-freeze.json`, `source-match.json`, `candidate-identity.json`: final frozen source and exact measured product; uncommitted WIP identified by per-file SHA.
- `checkpoint-note.json`: earlier checkpoints retained separately; never pooled into final samples.
- `phone-cleanup-receipt.json`: exact owned namespace cleanup, live-process/open-file inspection and independent du/df checks.

Full raw commands, stdout/stderr, source patches, compressed binaries and kernel fixtures remain at `/Volumes/Data2/swiftvm-middle-results-relocated-20261025/l1-dispatch-interrupt-20261109`. Final measurements live only in its `final/` directory. Historical checkpoints are preserved separately.

Implementation committed as `c750c99e6b815504b3f75713d7efdedc0038c368`. The original measurement records retain their pre-commit `head` and `committed:false` values; `submission.json` maps that frozen source snapshot to the implementation commit. No runtime source changed during submission, and no new benchmark observations were added.
