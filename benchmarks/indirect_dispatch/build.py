#!/usr/bin/env python3
"""Build fixed freestanding CALL/RET and JMP dispatch fixtures; does not run them."""
import argparse
from collections import Counter
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = Path('/Volumes/Data2/swiftvm-middle-results-relocated-20261025/indirect-dispatch-20261107/guest')
LLVM = Path('/opt/homebrew/opt/llvm@21/bin')
NDK = Path('/Users/swift/Library/Android/sdk/ndk/29.0.13846066/toolchains/llvm/prebuilt/darwin-x86_64/bin')
TABLE_SIZE = 1024
SEED = 0x5EED0114


@dataclass(frozen=True)
class Case:
    name: str
    branch: str
    targets: int
    stride: int
    pattern: str = 'round_robin'
    direct: bool = False


CASES = [
    Case('call-direct', 'call', 1, 4, direct=True),
    Case('call-mono', 'call', 1, 4),
    Case('call-4-spread', 'call', 4, 4),
    Case('call-16-spread', 'call', 16, 4),
    Case('call-4-collide64', 'call', 4, 64),
    Case('call-16-collide64', 'call', 16, 64),
    Case('call-64-permuted', 'call', 64, 4, 'seeded_permutation'),
    Case('jump-direct', 'jump', 1, 16, direct=True),
    Case('jump-mono', 'jump', 1, 16),
    Case('jump-4-spread16', 'jump', 4, 16),
    Case('jump-4-collide64', 'jump', 4, 64),
    Case('jump-64-permuted', 'jump', 64, 12, 'seeded_permutation'),
]


def identity(path):
    data = path.read_bytes()
    return {'path': str(path.resolve()), 'bytes': len(data),
            'sha256': hashlib.sha256(data).hexdigest()}


def run(argv, env):
    return subprocess.run([str(x) for x in argv], check=True, env=env,
                          capture_output=True, text=True).stdout


def sequence(case):
    values = [i % case.targets for i in range(TABLE_SIZE)]
    if case.pattern == 'seeded_permutation':
        state = SEED
        for i in range(len(values) - 1, 0, -1):
            state = (1664525 * state + 1013904223) & 0xffffffff
            j = state % (i + 1)
            values[i], values[j] = values[j], values[i]
    return values


def assembly(case, iterations, template, selectors):
    branch = ('    call target_0' if case.direct else '    call *%r11') if case.branch == 'call' else (
        '    jmp target_0' if case.direct else '    jmp *%r11')
    targets = []
    for i in range(case.targets):
        body = ['.type target_{0},@function'.format(i), 'target_{0}:'.format(i),
                '    mov ${0}, %al'.format(i + 1)]
        if case.branch == 'call':
            body += ['    ret', '    .fill {0},1,0x90'.format(case.stride - 3)]
        else:
            # A forced rel32 jump makes the target body exactly seven bytes.
            body += ['    .byte 0xe9', '    .long dispatch_resume - (. + 4)',
                     '    .fill {0},1,0x90'.format(case.stride - 7)]
        body += ['.size target_{0}, .-target_{0}'.format(i)]
        targets.extend(body)
    return (template.replace('@ITERATIONS@', str(iterations)).replace('@DISPATCH@', branch)
            .replace('@TARGETS@', '\n'.join(targets))
            .replace('@TABLE@', '\n'.join('    .quad target_{0}'.format(i) for i in selectors)))


def checksum(selectors, addresses, iterations):
    whole, tail = divmod(iterations, TABLE_SIZE)
    return_sum = whole * sum(i + 1 for i in selectors) + sum(i + 1 for i in selectors[:tail])
    per_period = 0
    for i in selectors:
        per_period ^= addresses[i]
    pointer_xor = per_period if whole & 1 else 0
    for i in selectors[:tail]:
        pointer_xor ^= addresses[i]
    return {'return_sum': return_sum & ((1 << 64) - 1), 'pointer_xor': pointer_xor,
            'checksum': (return_sum ^ pointer_xor) & ((1 << 64) - 1)}


def symbols(nm, elf, env):
    rows = run([nm, '--numeric-sort', '--defined-only', elf], env)
    found = {}
    for line in rows.splitlines():
        fields = line.split()
        if len(fields) == 3:
            found[fields[2]] = int(fields[0], 16)
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=DEFAULT_OUT)
    p.add_argument('--iterations', type=lambda s: int(s, 0), default=1 << 22)
    p.add_argument('--clang', type=Path, default=LLVM / 'clang')
    p.add_argument('--ld', type=Path, default=Path(shutil.which('ld.lld') or NDK / 'ld.lld'))
    p.add_argument('--nm', type=Path, default=LLVM / 'llvm-nm')
    args = p.parse_args()
    if not 0 < args.iterations < (1 << 63):
        p.error('--iterations must be in 1..2^63-1')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    scratch = args.out / 'tmp'
    scratch.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(scratch))
    template_path = HERE / 'guest.S.in'
    template = template_path.read_text()
    records = []
    for case in CASES:
        selectors = sequence(case)
        source, obj, elf = (args.out / (case.name + ext) for ext in ['.S', '.o', '.elf'])
        source.write_text(assembly(case, args.iterations, template, selectors))
        commands = [[args.clang, '--target=x86_64-linux-gnu', '-c', '-x', 'assembler', source, '-o', obj],
                    [args.ld, '-m', 'elf_x86_64', '--static', '--entry=_start', '--build-id=sha1',
                     '--image-base=0x400000', '-Ttext=0x401000', '-z', 'max-page-size=4096',
                     '-z', 'noexecstack', '-o', elf, obj]]
        for command in commands:
            run(command, env)
        syms = symbols(args.nm, elf, env)
        addresses = [syms['target_{0}'.format(i)] for i in range(case.targets)]
        if addresses != [addresses[0] + i * case.stride for i in range(case.targets)]:
            raise RuntimeError('assembler changed target spacing: ' + case.name)
        rows = [{'id': i, 'return_value': i + 1, 'symbol': 'target_{0}'.format(i),
                 'pc': pc, 'pc_hex': hex(pc), 'low6': pc & 63,
                 'svm_site_index': (pc >> 2) & 15, 'fex_l1_index': pc & 0xfffff}
                for i, pc in enumerate(addresses)]
        expected = checksum(selectors, addresses, args.iterations)
        record = {**asdict(case), 'iterations': args.iterations,
                  'expected_checksum': expected['checksum'], 'checksum_components': expected,
                  'warmup_iterations': 4096, 'warmup_checksum': checksum(selectors, addresses, 4096)['checksum'],
                  'rounds': 2, 'measured_round': 1, 'binary': identity(elf), 'assembly': identity(source),
                  'target_symbols': rows, 'svm_site_index_population': dict(sorted(Counter(r['svm_site_index'] for r in rows).items())),
                  'target_low6_population': dict(sorted(Counter(r['low6'] for r in rows).items())),
                  'sequence': selectors, 'sequence_target_counts': dict(sorted(Counter(selectors).items())),
                  'symbols': {k: syms[k] for k in ['_start', 'kernel', 'selection_loop', 'dispatch_site', 'dispatch_resume', 'measured_kernel_call', 'sequence_table']},
                  'commands': [[str(x) for x in command] for command in commands]}
        (args.out / (case.name + '.json')).write_text(json.dumps(record, indent=2) + '\n')
        records.append(record)
        obj.unlink()
    manifest = {'kind': 'freestanding_x86_64_indirect_dispatch',
                'iterations': args.iterations, 'warmup_iterations': 4096, 'table_entries': TABLE_SIZE,
                'seed': SEED, 'sequence_algorithm': 'round robin; permuted cases use fixed uint32 LCG Fisher-Yates over a balanced 1024-element sequence',
                'rounds': 2, 'measured_round': 1,
                'protocol': {'ready': 'READY <guest_gettid_decimal>\\n',
                             'gate': 'G byte on stdin', 'done': 'DONE <elapsed_ns_decimal> <checksum_decimal>\\n',
                             'acknowledge': 'X byte on stdin', 'successful_exit': 0, 'io_error_exit': 70,
                             'functional_stdin': 'GXGX',
                             'clock': 'guest CLOCK_MONOTONIC syscall 228; includes kernel call and return; excludes decimal output formatting',
                             'tid': 'guest gettid syscall 186; controller must confirm actual /proc/PID/task membership or enumerate the worker',
                             'warmup': '4096 kernel iterations before READY; round 0 fully traverses the same protocol/kernel callsite as measured round 1',
                             'kernel_reset': 'Every invocation resets selector index, return sum and pointer XOR; warmup and both rounds are independent'},
                'checksum': 'u64(sum of callee i+1 results) XOR u64(XOR of every loaded target pointer); direct controls retain the same pointer load/XOR and mono callee result',
                'layout': {'svm_site_index': '(target_pc >> 2) & 15', 'fex_l1_index': 'target_pc & 0xfffff',
                           'call_spread': 'One 64-byte bank alignment, then 4-byte entries: MOV AL,imm8;RET;NOP',
                           'jump_spread4': '16-byte entries; indices 0,4,8,12',
                           'jump_permuted64': '12-byte entries (MOV AL,imm8;JMP rel32;5 NOPs), spreads across all 16 slots',
                           'collide64': '64-byte entry stride; all target PCs have one SVM site-cache index'},
                'inputs': {'builder': identity(Path(__file__)), 'template': identity(template_path)},
                'tools': {'clang': identity(args.clang), 'clang_version': run([args.clang, '--version'], env).splitlines()[0],
                          'ld': identity(args.ld), 'ld_version': run([args.ld, '--version'], env).strip(),
                          'nm': identity(args.nm)},
                'qualification': 'Built only. No emulator, phone, PMU, benchmark, or runtime test was run by this builder. Same ELF input is required for both runtimes.',
                'cases': records}
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    scratch.rmdir()
    print(json.dumps({'out': str(args.out), 'cases': len(records), 'iterations': args.iterations,
                      'rounds': 2, 'manifest': str(args.out / 'manifest.json')}))


if __name__ == '__main__':
    main()
