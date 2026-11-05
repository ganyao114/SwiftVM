from pathlib import Path
import datetime
import difflib
import gzip
import hashlib
import json
import shutil
import subprocess

B = Path(__file__).resolve().parent
A = B / 'attempt3'
R = Path('/Volumes/Data2/SwiftVM-submit-20261104')
OLD = B.parent / 'compile-owner-optimization-20261105/buffers-publish'
E = R / 'docs/evidence/continuation-reuse-20261105'
E.mkdir(exist_ok=False)
read = lambda path: json.loads(path.read_text())
sha = lambda data: hashlib.sha256(data).hexdigest()

manifest = read(A / 'source-manifest.json')
assert manifest['digest'] == 'a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52'
for row in manifest['files']:
    assert sha((R / row['path']).read_bytes()) == row['sha256']
    assert sha((A / 'source' / row['path']).read_bytes()) == row['sha256']
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=R, text=True).strip() == manifest['base_commit']
summary = read(A / 'summary.json')
assert summary['acceptance']['failures'] == [dict(case='x87_bench', arm='block', metric='compile', ratio=summary['ratios']['x87_bench']['block']['compile'], limit=2.0)]
audit = read(B / 'audit-accepted-candidate/analysis.json')
assert audit['inputs']['production_source_digest'] == manifest['digest']
assert not audit['oracle']['timing_used_for_qor']
assert summary['phone_cleanup']['remaining_bytes'] == 0

def copy(src, dest):
    target = E / dest
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, target)

def gz(src, dest):
    target = E / dest
    target.parent.mkdir(parents=True, exist_ok=True)
    data = src.read_bytes()
    with target.open('wb') as f:
        with gzip.GzipFile(fileobj=f, mode='wb', mtime=0) as z:
            z.write(data)
    assert gzip.decompress(target.read_bytes()) == data

for name in ['summary.json', 'compile-records.json', 'runtime-records.json', 'compile-breakdown.json',
             'comparison.json', 'source-manifest.json', 'inputs.json', 'identity.json', 'environment.json',
             'cases.json', 'smoke-summary.json', 'mode-probe-summary.json', 'phone-cleanup-receipt.json',
             'host-build.log', 'android-build.log', 'host-ctest.log', 'run.py']:
    copy(A / name, name)
for name in ['analyze.py', 'augment-analysis.py', 'package-evidence.py', 'final-ordinary-comparison.json',
             'final-ordinary-comparison.md', 'a2-rejected-ordinary-comparison.json',
             'a2-rejected-ordinary-comparison.md', 'final-code-review.md', 'baseline-compression-receipt.json']:
    copy(B / name, name)
for name in ['cause.json', 'report.md', 'receipt.json', 'analyze-cause.py', 'request-roles.jsonl']:
    copy(B / 'cause' / name, 'cause/' + name)
for directory, names in {
    'audit-accepted-candidate': ['analysis.json', 'report.md', 'unit-coverage.jsonl', 'analyze.py',
                               'analysis-receipt.json', 'receipt.json', 'execution.json',
                               'build-diagnostic.py', 'diagnostic.patch', 'coremark20-function.stdout'],
    'audit-final': ['analysis.json', 'report.md', 'analysis-receipt.json', 'execution.json',
                    'func-tests-fallback-reasons.json', 'func-tests-fallback-reasons.md',
                    'func-tests-execution.json'],
    'audit-after': ['execution.json', 'fallback-reasons.json', 'fallback-reasons.md'],
    'x87-paired-confirmation': ['plan.json', 'records.json', 'summary.json'],
    'attempt1': ['source-manifest.json', 'initial-cost.json', 'phone-cleanup-receipt.json'],
    'attempt2': ['source-manifest.json', 'rejection-receipt.json', 'compile-records.json',
                 'runtime-records.json', 'phone-cleanup-receipt.json'],
}.items():
    for name in names:
        copy(B / directory / name, directory + '/' + name)
for directory, names in {
    'audit-accepted-candidate': ['coremark20-function.stderr'],
    'audit-final': ['coremark20-function.stderr', 'func-tests-function.stderr'],
    'audit-after': ['coremark20-function.stderr'],
}.items():
    for name in names:
        gz(B / directory / name, directory + '/' + name + '.gz')

def raw_outputs(source, dest):
    rows = []
    for src in sorted(source.iterdir()):
        if not src.is_file() or src.suffix not in ['.stdout', '.stderr']:
            continue
        data = src.read_bytes()
        rows.append(dict(path=src.name, bytes=len(data), sha256=sha(data), content=data.decode()))
    (E / dest).write_text(''.join(json.dumps(row, ensure_ascii=False) + '\n' for row in rows))
    return len(rows)

raw_count = raw_outputs(A, 'raw-output.jsonl')
raw_outputs(B / 'x87-paired-confirmation', 'x87-paired-confirmation/raw-output.jsonl')
changes = []
changed_files = []
for row in manifest['files']:
    rel = row['path']
    old, new = (OLD / 'source' / rel).read_text(), (A / 'source' / rel).read_text()
    if old != new:
        changed_files.append(rel)
        changes.extend(difflib.unified_diff(old.splitlines(keepends=True), new.splitlines(keepends=True),
                                            fromfile='a/' + rel, tofile='b/' + rel))
(E / 'changes-from-buffer-reuse.patch').write_text(''.join(changes))
assert sha(gzip.decompress((A / 'candidate.gz').read_bytes())) == summary['inputs']['candidate']['sha256']
receipt = {
    'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'kind': 'continuation-reuse-task-evidence-original-suite-and-confirmation-kept-separate',
    'base_commit': manifest['base_commit'], 'working_tree_modified': True, 'committed_or_pushed': False,
    'source_worktree': str(R), 'source_snapshot': str(A / 'source'),
    'source_digest': manifest['digest'], 'source_files': len(manifest['files']),
    'source_current_and_frozen_match': True, 'changed_files_from_buffer_reuse': changed_files,
    'ordinary_candidate_sha256': summary['inputs']['candidate']['sha256'],
    'ordinary_candidate_archive': str(A / 'candidate.gz'),
    'diagnostic_candidate_sha256': audit['inputs']['diagnostic_binary_sha256'],
    'runtime_samples': 72, 'compile_samples': 72, 'raw_output_files': raw_count,
    'all_output_oracles_valid': True, 'original_fixed_suite_gates_passed': False,
    'original_gate_failures': summary['acceptance']['failures'],
    'independent_x87_confirmation': read(B / 'x87-paired-confirmation/summary.json'),
    'confirmation_does_not_overwrite_original_suite': True,
    'all_diagnostic_timing_excluded_from_qor': True,
    'phone_cleanup': summary['phone_cleanup'],
    'archives': str(B),
    'files': {str(p.relative_to(E)): dict(bytes=p.stat().st_size, sha256=sha(p.read_bytes()))
              for p in sorted(E.rglob('*')) if p.is_file()},
}
(E / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({k: v for k, v in receipt.items() if k not in ['files', 'independent_x87_confirmation']}, indent=2))
