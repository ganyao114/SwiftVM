# ARM64 code generation optimization

See the [implementation and results](../../codegen-quality-optimization-2026-11-06.md).
The candidate is an uncommitted working tree based on `ebf55740`, identified by
the 282-file source digest and runner SHA in `receipt.json`.

This is a fixed five-arm, five-round comparison: previous Block/Function,
candidate Block/Function and fixed FEX across eight inputs. All 200 runtime and
200 separate compile samples are retained. `comparisons.json` reports changes
against the same-batch baseline and ratios against the same-batch FEX.
Runtime/compile summaries preserve all five values and their min/max; component
ledgers close each original compile window without summing nested inclusive
stages. SVM residuals are not independent attribution, and the SVM/FEX final
native-release boundary difference remains explicit in the report.

Static native listings reconstruct actual guest loop inputs through production
IR/optimization/allocation/encoding. They are not a bytewise capture of runtime
RX. Full IR/Home dumps and probe sources remain in the raw directory, alongside
the compressed source and both executables:

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/codegen-quality-optimization-20261106`

The current behavior tests cover the existing integer, linked flags and both
scope conditional near/far/unlink contracts. Host and sanitizer logs and device
summaries are retained. Functional and benchmark phone namespaces were each
checked idle and removed; cleanup receipts account for the released space.

CoreMark/func_tests Function and x87 runtime changes that did not improve are
kept. The x87 Block compile budget is still open. No repeated rounds were used
to replace slower samples or reach a threshold.
