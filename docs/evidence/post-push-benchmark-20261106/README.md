# Post-push benchmark: 2026-11-06

Subject: `ebf557408e14aef165b28bd7f0f3ebfc0ae3e918`.
The [report](../../post-push-benchmark-2026-11-06.md) contains the measured gaps,
timing boundaries, PMU interpretation and the two unmet x87 compile gates.

The plan fixes eight inputs, three arms and five fresh launches per arm/input.
All 120 runtime and 120 separate compile observations are retained; no samples
were replaced, added to reach thresholds, or pooled with earlier checkpoints.
`summary.json` includes all five values, medians, min/max and individual gates.
`compile-breakdown.json` selects one real median-total sample per arm and retains
all component records; SVM `other` is a residual, not an independently measured
cause. Final native release boundaries still differ between SVM and FEX.

`plan.json`, `identity.json`, `source-manifest.json`, `inputs.json`, the deployment
SHA outputs and `receipt.json` bind source, executables, device and results.
The preserved harness and analyzer reproduce the protocol and aggregation.
`resource-comparison-to-preformat.json` records six unchanged resource statistics
for each of the sixteen workload/scope combinations; it is not a machine-code
or full semantic equivalence proof.

Full stdout/stderr, PMU text, wrapper status, candidate/source compressed
snapshots, and cleanup logs remain on Data2:

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/post-push-benchmark-20261106`

The phone namespace is absent, with zero active processes/open files and no
cleanup errors. Production sources were not modified during this measurement.
