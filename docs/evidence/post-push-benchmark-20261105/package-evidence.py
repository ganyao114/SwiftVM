from pathlib import Path
import datetime
import gzip
import hashlib
import json
import shutil
import subprocess

P = Path(__file__).resolve().parent
R = Path('/Volumes/Data2/SwiftVM-submit-20261104')
E = R / 'docs/evidence/post-push-benchmark-20261105'
read = lambda path: json.loads(path.read_text())
sha = lambda data: hashlib.sha256(data).hexdigest()
plan, manifest = read(P / 'plan.json'), read(P / 'source-manifest.json')
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=R, text=True).strip() == plan['subject_commit']
assert subprocess.check_output(['git', 'ls-remote', '--heads', 'origin', plan['remote_ref']], cwd=R, text=True).split()[0] == plan['subject_commit']
assert plan['repetitions_per_case_arm'] == 5
assert sha((P / 'run.py').read_bytes()) == plan['harness_sha256']
assert sha((P / 'source-manifest.json').read_bytes()) == plan['source_manifest_sha256']
for row in manifest['files']:
    assert sha((R / row['path']).read_bytes()) == row['sha256'] == sha((P / 'source' / row['path']).read_bytes())
    committed = subprocess.check_output(['git', 'show', plan['subject_commit'] + ':' + row['path']], cwd=R)
    assert sha(committed) == row['sha256']
for mode in ['compile', 'runtime']:
    rows = read(P / (mode + '-records.json'))
    assert len(rows) == 120 and all(row['valid'] for row in rows)
    assert {(row['case'], row['arm'], row['round']) for row in rows} == {
        (case, arm, rep) for case in plan['workloads'] for arm in plan['arms'] for rep in range(5)}
cleanup = read(P / 'phone-cleanup-receipt.json')
assert cleanup['remaining_bytes'] == 0 and not cleanup['active_processes'] and not cleanup['open_files'] and not cleanup['errors']
E.mkdir(exist_ok=False)
for name in ['plan.json', 'identity.json', 'source-manifest.json', 'inputs.json', 'environment.json',
             'cases.json', 'commands.json', 'smoke-summary.json', 'mode-probe-summary.json',
             'compile-records.json', 'runtime-records.json', 'summary.json', 'compile-breakdown.json',
             'comparison.json', 'phone-cleanup-receipt.json', 'host-build.log', 'android-build.log',
             'run.py', 'analyze.py', 'render-report.py', 'package-evidence.py']:
    shutil.copyfile(P / name, E / name)
raw_count = 0
with (E / 'raw-output.jsonl').open('w') as out:
    for path in sorted(P.iterdir()):
        if path.is_file() and path.suffix in ['.stdout', '.stderr']:
            data = path.read_bytes()
            row = dict(path=path.name, bytes=len(data), sha256=sha(data), content=data.decode())
            out.write(json.dumps(row, ensure_ascii=False) + '\n')
            raw_count += 1
runner = read(P / 'inputs.json')['candidate']
data = Path(runner['path']).read_bytes()
assert sha(data) == runner['sha256'] == '13411690d78fa72df6fc84c61004bfe7c36a2060cc3a9a16cb22dcbfca9d77eb'
archive = P / 'candidate.gz'
assert not archive.exists()
with archive.open('wb') as f:
    with gzip.GzipFile(fileobj=f, mode='wb', mtime=0) as z:
        z.write(data)
assert sha(gzip.decompress(archive.read_bytes())) == runner['sha256']
receipt = dict(
    utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    kind='post-push-fixed-five-round-benchmark-anchor',
    subject_commit=plan['subject_commit'], code_commit=plan['code_commit'],
    remote_ref=plan['remote_ref'], remote_subject_verified=True,
    source_digest=manifest['digest'], source_files=118,
    source_current_frozen_and_committed_match=True,
    ordinary_candidate_sha256=runner['sha256'], ordinary_candidate_archive=str(archive),
    FEX_revision=plan['FEX_revision'], repetitions=5, runtime_samples=120, compile_samples=120,
    all_output_oracles_valid=True, all_samples_retained=True,
    prior_samples_pooled=False, threshold_driven_followup=False,
    acceptance=read(P / 'summary.json')['acceptance'],
    phone_cleanup=cleanup, raw_output_files=raw_count,
    artifacts=str(P),
    files={str(path.relative_to(E)): dict(bytes=path.stat().st_size, sha256=sha(path.read_bytes()))
           for path in sorted(E.iterdir()) if path.is_file()})
(E / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({key: value for key, value in receipt.items() if key != 'files'}, indent=2))
