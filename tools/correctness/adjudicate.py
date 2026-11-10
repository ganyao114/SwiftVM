#!/usr/bin/env python3
"""Reclassify oracle mismatches using host-CPU observations.

Reads a run controller's results.json, and for every semantic/exception
mismatch whose case has a native observation (keyed by case sha256), compares
the DUT's actual state against the host CPU instead of the software oracle.
"""
import argparse
import hashlib
import json
import subprocess
import tempfile
from collections import Counter
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--cases', type=Path, required=True)
    parser.add_argument('--native-dir', type=Path, required=True)
    parser.add_argument('--dut', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()

    results = json.loads(args.results.read_text())
    counts = Counter(results.get('counts', {}))
    reclassified = []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        for failure in results.get('failures', []):
            comparison = failure.get('comparison', {})
            if comparison.get('category') not in ('semantic_mismatch', 'exception_mismatch'):
                continue
            case = args.cases / (failure['case'] + '.svmcase')
            if not case.exists():
                continue
            digest = hashlib.sha256(case.read_bytes()).hexdigest()
            native = args.native_dir / (digest + '.json')
            if not native.exists():
                continue
            actual = scratch / 'actual.json'
            actual.write_text(json.dumps(failure['actual']))
            comparison_path = scratch / 'comparison.json'
            subprocess.run([str(args.dut), 'compare', '--case', str(case),
                            '--expected', str(native), '--actual', str(actual),
                            '--out', str(comparison_path)],
                           check=True, capture_output=True)
            checked = json.loads(comparison_path.read_text())
            if checked.get('equal') and checked.get('comparable'):
                counts[comparison['category']] -= 1
                counts['native_verified_gap'] += 1
                reclassified.append(failure['case'])
    summary = {'counts': dict(counts), 'native_verified': reclassified,
               'cases': results.get('cases')}
    text = json.dumps(summary, indent=1, sort_keys=True)
    if args.out:
        args.out.write_text(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
