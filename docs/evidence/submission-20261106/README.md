# Submission validation

These files describe the formatted source, not a new performance measurement.
See [the validation record](../../submission-validation-2026-11-06.md).

`source-manifest.json` records the host/Android dependency union at validation
time. `format-checks.json` records formatter versions and unchanged semantic
inventory. Build/CTest logs and phone summaries describe the actual validation;
`phone-inputs.json` and `remote-identities.stdout` bind the device inputs.

The initial smoke verdict incorrectly required the official CoreMark validation
message for a short 20-iteration CRC smoke. Both original outputs, the initial
summary and the explicit oracle correction are retained. The corrected summary
uses the same exit0/CRC0x4983 contract as the earlier smoke harness. No samples
were rerun or replaced, and these short runs are not CoreMark scores.

Full outputs, census snapshots and the phone harness are retained at:

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/format-submit-20261106`

The phone cleanup receipt confirms that this round's namespace is absent and
has no remaining active writer or open file. Historical benchmark evidence is
left unchanged.
