from pathlib import Path
import datetime
import gzip
import hashlib
import json
import re
import shutil
import subprocess

B = Path(__file__).resolve().parent
R = Path('/Volumes/Data2/SwiftVM-submit-20261104')
A = B / 'attempt3'
E = R / 'docs/evidence/continuation-reuse-20261105'
read = lambda p: json.loads(p.read_text())
sha = lambda data: hashlib.sha256(data).hexdigest()
receipt = read(E / 'receipt.json')
shutil.copyfile(Path(__file__), E / 'verify-closeout.py')
data = (E / 'verify-closeout.py').read_bytes()
receipt['files']['verify-closeout.py'] = dict(bytes=len(data), sha256=sha(data))
(E / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
for rel, identity in receipt['files'].items():
    data = (E / rel).read_bytes()
    assert len(data) == identity['bytes'] and sha(data) == identity['sha256'], rel

manifest = read(A / 'source-manifest.json')
digest = hashlib.sha256()
for identity in manifest['files']:
    rel, expected = identity['path'], identity['sha256']
    assert sha((R / rel).read_bytes()) == expected == sha((A / 'source' / rel).read_bytes()), rel
    digest.update((rel + '\0' + expected + '\n').encode())
assert digest.hexdigest() == manifest['digest'] == receipt['source_digest']
assert len(manifest['files']) == 118
for mode in ['compile', 'runtime']:
    records = read(E / (mode + '-records.json'))
    assert len(records) == 72 and all(row['valid'] for row in records)
    assert {(row['case'], row['arm'], row['round']) for row in records} == {
        (case['name'], arm, rep) for case in read(E / 'cases.json')
        for arm in ['block', 'function', 'fex'] for rep in range(3)}
    assert (E / (mode + '-records.json')).read_bytes() == (A / (mode + '-records.json')).read_bytes()
raw_count = 0
for folder, target in [(A, E / 'raw-output.jsonl'),
                       (B / 'x87-paired-confirmation', E / 'x87-paired-confirmation/raw-output.jsonl')]:
    for line in target.read_text().splitlines():
        row = json.loads(line)
        data = row['content'].encode()
        assert len(data) == row['bytes'] and sha(data) == row['sha256']
        assert data == (folder / row['path']).read_bytes()
        raw_count += 1
audit = read(E / 'audit-accepted-candidate/analysis.json')
trace = gzip.decompress((E / 'audit-accepted-candidate/coremark20-function.stderr.gz').read_bytes())
assert sha(trace) == audit['inputs']['trace_sha256']
assert sha(gzip.decompress((A / 'candidate.gz').read_bytes())) == receipt['ordinary_candidate_sha256']
assert sha(Path(read(A / 'inputs.json')['candidate']['path']).read_bytes()) == receipt['ordinary_candidate_sha256']
for row in read(B / 'local-temporary-cleanup-receipt.json')['archived']:
    archive = Path(row.get('gzip_path', row.get('retained_archive')))
    assert sha(gzip.decompress(archive.read_bytes())) == row['original_sha256']
    assert not Path(row['original_path']).exists()
for doc in ['README.md', 'ARCHITECTURE.md', 'docs/function-entry-reuse-2026-11-05.md']:
    path = R / doc
    for link in re.findall(r'\]\(([^)]+)\)', path.read_text()):
        if link.startswith(('http:', 'https:', '#')):
            continue
        target = link.split('#')[0]
        assert (path.parent / target).exists(), (doc, link)
assert read(A / 'phone-cleanup-receipt.json')['remaining_bytes'] == 0
assert len(read(B / 'x87-paired-confirmation/records.json')) == 21
assert read(B / 'x87-paired-confirmation/summary.json')['no_optional_stopping']
assert len(read(A / 'summary.json')['acceptance']['failures']) == 1
assert not read(A / 'summary.json')['acceptance']['passed']
subprocess.run(['git', 'diff', '--check'], cwd=R, check=True)
head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=R, text=True).strip()
assert head == manifest['base_commit']
status = subprocess.check_output(['git', 'status', '--short'], cwd=R, text=True)
(B / 'final-git-status.txt').write_text(status)
with (B / 'final-tracked-wip.patch').open('wb') as f:
    subprocess.run(['git', 'diff', '--binary'], cwd=R, stdout=f, check=True)
shutil.copyfile(R / 'docs/function-entry-reuse-2026-11-05.md', B / 'function-entry-reuse-2026-11-05.md')
final = dict(
    utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    task_status='duplicate_construction_fix_implemented_measured_and_documented',
    overall_performance_status='original_fixed_suite_47_of_48_limits_passed_not_stable_full_acceptance',
    head=head, committed_or_pushed=False,
    source_digest=manifest['digest'], source_files=118,
    evidence_files_verified=len(receipt['files']), raw_outputs_verified=raw_count,
    runtime_samples=72, compile_samples=72, output_oracles_valid=True,
    independent_x87_paired_samples=21, original_gate_failure_retained=True,
    source_current_frozen_and_candidate_match=True,
    function_audit='audit-accepted-candidate/analysis.json',
    report=str(R / 'docs/function-entry-reuse-2026-11-05.md'),
    report_sha256=sha((R / 'docs/function-entry-reuse-2026-11-05.md').read_bytes()),
    evidence_receipt_sha256=sha((E / 'receipt.json').read_bytes()),
    tracked_wip_sha256=sha((B / 'final-tracked-wip.patch').read_bytes()),
    phone_cleanup=read(A / 'phone-cleanup-receipt.json'),
    local_temporary_cleanup=read(B / 'local-temporary-cleanup-receipt.json'),
    git_diff_check=True, errors=[])
(B / 'closeout-receipt.json').write_text(json.dumps(final, indent=2) + '\n')
print(json.dumps({k: v for k, v in final.items() if k not in ['local_temporary_cleanup', 'phone_cleanup']}, indent=2))
