#!/usr/bin/env python3
"""Runs the static guest programs natively and through the rebuilt runtime.

Compares stdout and exit status for the native, JIT and bounded-AOT paths,
then records wall time and the runtime report for each mode. A bounded AOT
artifact prewarms a small entry set; the JIT fills the remaining entries, so
the compile stays cheap and remains valid for the current build identity.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

PROGRAMS = {
    'hello': {'args': ['a', 'b']},
    'syscalls': {},
    'compute': {},
    'fp': {},
    'tiny': {},
    'bench_int': {},
    'bench_fp': {},
}
ELAPSED = re.compile(r'elapsed_ns=\d+')


def normalized(text):
    return ELAPSED.sub('elapsed_ns=<measured>', text)


def report(stderr):
    for line in stderr.splitlines():
        if '"measurement_kind"' in line:
            try:
                return json.loads(line)
            except ValueError:
                return {}
    return {}


def run_local(command, timeout, cwd=None):
    started = time.monotonic()
    process = subprocess.run(command, capture_output=True, text=True, timeout=timeout, cwd=cwd)
    return {
        'command': command,
        'returncode': process.returncode,
        'stdout': process.stdout,
        'stderr': process.stderr,
        'wall_ns': int((time.monotonic() - started) * 1e9),
    }


class Device:
    def __init__(self, serial):
        self.prefix = ['adb', '-s', serial]

    def shell(self, command, timeout):
        return subprocess.run(self.prefix + ['shell', command], capture_output=True,
                              text=True, timeout=timeout)

    def push(self, local, remote):
        subprocess.run(self.prefix + ['push', str(local), remote],
                       check=True, capture_output=True, text=True)

    def run(self, directory, command, timeout):
        out, err = f'{directory}/.stdout', f'{directory}/.stderr'
        quoted = ' '.join(f"'{part}'" for part in command)
        started = time.monotonic()
        result = self.shell(
                f'cd {directory} && {quoted} > {out} 2> {err}; echo RC:$?', timeout)
        wall_ns = int((time.monotonic() - started) * 1e9)
        code = result.stdout.strip().split('RC:')[-1]
        stdout = self.shell(f'cat {out}', timeout).stdout
        stderr = self.shell(f'cat {err}', timeout).stdout
        return {'command': command, 'returncode': int(code or 255), 'stdout': stdout,
                'stderr': stderr, 'wall_ns': wall_ns}

    def cleanup(self, directory):
        self.shell(f'rm -rf {directory}', 30)


def compare(reference, result):
    return (reference['returncode'] == result['returncode'] and
            normalized(reference['stdout']) == normalized(result['stdout']))


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=here / 'bin')
    parser.add_argument('--runner', type=Path, required=True,
                        help='svm_rebuild_linux for the current host or device')
    parser.add_argument('--aot', type=Path,
                        help='svm_aot used for the AOT mode')
    parser.add_argument('--out', type=Path, default=here / 'out')
    parser.add_argument('--mode', choices=('jit', 'aot', 'all'), default='all')
    parser.add_argument('--aot-max', type=int, default=16)
    parser.add_argument('--adb', metavar='SERIAL', help='run on an Android device')
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()

    programs = [name for name in PROGRAMS if (args.bin_dir / name).exists()]
    if not programs:
        parser.error(f'no programs in {args.bin_dir}; run build.sh first')
    args.out.mkdir(parents=True, exist_ok=True)
    summary = {'programs': {}, 'modes': []}
    device = None
    remote = '/data/local/tmp/svm-programs'
    reference = {}
    if args.adb:
        device = Device(args.adb)
        device.cleanup(remote)
        device.shell(f'mkdir -p {remote}', 30)
        for name in programs:
            device.push(args.bin_dir / name, f'{remote}/{name}')
        device.push(args.runner, f'{remote}/svm_rebuild_linux')
        if args.aot:
            device.push(args.aot, f'{remote}/svm_aot')
        device.shell(f'chmod 755 {remote}/*', 30)
        runner, aot = f'./svm_rebuild_linux', f'./svm_aot'
    else:
        runner, aot = str(args.runner.resolve()), str(args.aot.resolve()) if args.aot else ''

    try:
        for name in programs:
            entry = {}
            argument = PROGRAMS[name].get('args', [])
            if device:
                native = device.run(remote, [f'./{name}'] + argument, args.timeout)
            else:
                native = run_local([str(args.bin_dir / name)] + argument, args.timeout,
                                   cwd=str(args.out))
            reference[name] = native
            entry['native'] = {k: native[k] for k in ('returncode', 'wall_ns')}
            if args.mode in ('jit', 'all'):
                if device:
                    jit = device.run(remote, [runner, f'./{name}'] + argument, args.timeout)
                else:
                    jit = run_local([runner, str(args.bin_dir / name)] + argument, args.timeout,
                                    cwd=str(args.out))
                entry['jit'] = {'match': compare(native, jit),
                                'returncode': jit['returncode'], 'wall_ns': jit['wall_ns'],
                                'report': report(jit['stderr'])}
            if args.mode in ('aot', 'all'):
                if not aot:
                    parser.error('--aot is required for the AOT mode')
                artifact = f'{remote}/{name}.aot' if device else str(args.out / f'{name}.aot')
                if device:
                    build = device.run(remote, [aot, 'compile', f'./{name}', '-o', artifact,
                                                '--max', str(args.aot_max), '--no-sweep'],
                                       max(args.timeout, 300))
                    launch = device.run(remote, [aot, 'run', '--aot', artifact, '--',
                                                 f'./{name}'] + argument, args.timeout)
                else:
                    build = run_local([aot, 'compile', str(args.bin_dir / name), '-o', artifact,
                                       '--max', str(args.aot_max), '--no-sweep'],
                                      max(args.timeout, 300))
                    launch = run_local([aot, 'run', '--aot', artifact, '--',
                                        str(args.bin_dir / name)] + argument, args.timeout,
                                       cwd=str(args.out))
                entry['aot'] = {'compiled': build['returncode'] == 0,
                                'match': compare(native, launch),
                                'returncode': launch['returncode'], 'wall_ns': launch['wall_ns'],
                                'report': report(launch['stderr'])}
            entry['pass'] = all(mode.get('match', True) for mode in
                                (entry.get('jit', {}), entry.get('aot', {})))
            summary['programs'][name] = entry
    finally:
        if device:
            device.cleanup(remote)

    summary['modes'] = [mode for mode in ('jit', 'aot') if mode == args.mode or args.mode == 'all']
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=1) + '\n')
    failed = [name for name, entry in summary['programs'].items() if not entry['pass']]
    for name, entry in summary['programs'].items():
        modes = ' '.join(f'{mode}={"ok" if entry[mode]["match"] else "MISMATCH"}'
                         for mode in ('jit', 'aot') if mode in entry)
        print(f'{name}: {"PASS" if entry["pass"] else "FAIL"} {modes}')
    if failed:
        print('failed: ' + ', '.join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
