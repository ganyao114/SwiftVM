from pathlib import Path
import datetime
import gzip
import hashlib
import json
import math
import re
import shutil
import statistics
import subprocess

P = Path(__file__).resolve().parent
R = Path('/Volumes/Data2/SwiftVM-submit-20261104')
E = R / 'docs/evidence/post-push-benchmark-20261105'
read = lambda path: json.loads(path.read_text())
sha = lambda data: hashlib.sha256(data).hexdigest()
plan, summary = read(P / 'plan.json'), read(E / 'summary.json')
receipt = read(E / 'receipt.json')
shutil.copyfile(Path(__file__), E / 'verify-closeout.py')
data = (E / 'verify-closeout.py').read_bytes()
receipt['files']['verify-closeout.py'] = dict(bytes=len(data), sha256=sha(data))
(E / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
for name, identity in receipt['files'].items():
    data = (E / name).read_bytes()
    assert len(data) == identity['bytes'] and sha(data) == identity['sha256'], name
raw_count = 0
for line in (E / 'raw-output.jsonl').read_text().splitlines():
    row = json.loads(line)
    data = row['content'].encode()
    assert len(data) == row['bytes'] and sha(data) == row['sha256']
    assert data == (P / row['path']).read_bytes()
    raw_count += 1
assert raw_count == receipt['raw_output_files']
manifest = read(E / 'source-manifest.json')
digest = hashlib.sha256()
for row in manifest['files']:
    path, expected = row['path'], row['sha256']
    committed = subprocess.check_output(['git', 'show', plan['subject_commit'] + ':' + path], cwd=R)
    assert sha(committed) == sha((R / path).read_bytes()) == sha((P / 'source' / path).read_bytes()) == expected
    digest.update((path + '\0' + expected + '\n').encode())
assert digest.hexdigest() == plan['source_digest'] == summary['source_closure']['digest']
assert len(manifest['files']) == 118
assert sha((P / 'run.py').read_bytes()) == plan['harness_sha256']
assert sha((P / 'source-manifest.json').read_bytes()) == plan['source_manifest_sha256']
runtime, compile_rows = read(E / 'runtime-records.json'), read(E / 'compile-records.json')
cartesian = {(case, arm, rep) for case in plan['workloads'] for arm in plan['arms'] for rep in range(5)}
for rows in [runtime, compile_rows]:
    assert len(rows) == 120 and all(row['valid'] for row in rows)
    assert {(row['case'], row['arm'], row['round']) for row in rows} == cartesian
for row in compile_rows:
    record = row['record']
    if row['arm'] == 'fex':
        assert record['qualified'] and record['closed']
        assert sum(stage['remaining_ticks'] for stage in record['stages']) == record['root_ticks']
        value = (record['root_ticks'] - next(stage['ticks'] for stage in record['stages']
                                             if stage['name'] == 'byte_decode')) * 1e9 / record['cntfrq_hz']
    else:
        value = record['compile_ns'] - record['compile_decode_ns']
    assert math.isclose(row['compile_excluding_byte_ns'], value, abs_tol=1)
failures = []
for case in plan['workloads']:
    medians = {}
    for arm in plan['arms']:
        rr = [row for row in runtime if row['case'] == case and row['arm'] == arm]
        cc = [row for row in compile_rows if row['case'] == case and row['arm'] == arm]
        medians[arm] = dict(seconds=statistics.median(row['seconds'] for row in rr),
                            cycles=statistics.median(row['pmu']['cpu-cycles:u'] for row in rr),
                            compile=statistics.median(row['compile_excluding_byte_ns'] for row in cc))
    for arm in ['block', 'function']:
        for metric, limit in [('seconds', 1.1), ('cycles', 1.1), ('compile', 2.0)]:
            ratio = medians[arm][metric] / medians['fex'][metric]
            assert math.isclose(ratio, summary['ratios'][case][arm][metric], abs_tol=1e-12)
            if ratio > limit:
                failures.append(dict(case=case, arm=arm, metric=metric, ratio=ratio, limit=limit))
assert failures == summary['acceptance']['failures'] == []
assert summary['acceptance']['passed_checks'] == 48 and summary['acceptance']['n'] == 5
runner = read(E / 'inputs.json')['candidate']
assert sha(Path(runner['path']).read_bytes()) == sha(gzip.decompress((P / 'candidate.gz').read_bytes())) == runner['sha256']
for name in ['README.md', 'ARCHITECTURE.md', 'docs/post-push-benchmark-2026-11-05.md']:
    path = R / name
    for target in re.findall(r'\]\(([^)]+)\)', path.read_text()):
        if not target.startswith(('http:', 'https:', '#')):
            assert (path.parent / target.split('#')[0]).exists(), (name, target)
cleanup = read(E / 'phone-cleanup-receipt.json')
assert cleanup['remaining_bytes'] == 0 and not cleanup['active_processes'] and not cleanup['open_files'] and not cleanup['errors']
subprocess.run(['git', 'diff', '--check'], cwd=R, check=True)
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=R, text=True).strip() == plan['subject_commit']
report = R / 'docs/post-push-benchmark-2026-11-05.md'
shutil.copyfile(report, P / report.name)
result = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              status='post-push-benchmark-complete', subject_commit=plan['subject_commit'],
              code_commit=plan['code_commit'], source_digest=plan['source_digest'],
              runtime_samples=120, compile_samples=120, repetitions=5,
              all_samples_valid=True, all48_limits_passed=True,
              original_samples_retained=True, prior_samples_pooled=False,
              evidence_files_verified=len(receipt['files']), raw_outputs_verified=raw_count,
              source_current_frozen_and_committed_match=True,
              receipt_sha256=sha((E / 'receipt.json').read_bytes()),
              report=str(report), report_sha256=sha(report.read_bytes()),
              phone_cleanup=cleanup, errors=[])
(P / 'closeout-receipt.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({key: value for key, value in result.items() if key != 'phone_cleanup'}, indent=2))
