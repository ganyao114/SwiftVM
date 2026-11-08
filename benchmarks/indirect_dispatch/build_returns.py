#!/usr/bin/env python3
"""Build the paired four-caller/shared-RET fixtures without changing the baseline."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import shutil
import sys

# Reuse the frozen builder's hashing, tool invocation, symbol reading and
# checksum helpers without creating __pycache__ in the source checkout.
sys.dont_write_bytecode = True
import build as base

DEFAULT_OUT = Path('/Volumes/Data2/swiftvm-middle-results-relocated-20261025/indirect-dispatch-20261107/return-fanin/guest')


def bank(spread):
    lines = ['.p2align 8', '.global caller_bank', 'caller_bank:']
    for i in range(4):
        entry_offset = i * 256 + 4 * i
        return_offset = i * 256 + 80 + (4 * i if spread else 0)
        lines += [f'    .org caller_bank + {entry_offset}, 0x90',
                  f'.type caller_entry_{i},@function', f'caller_entry_{i}:',
                  '    .byte 0xeb', f'    .byte call_{i} - (. + 1)',
                  f'.size caller_entry_{i}, .-caller_entry_{i}',
                  f'    .org caller_bank + {return_offset - 5}, 0x90',
                  f'.type call_{i},@function', f'call_{i}:',
                  '    call shared_callee', f'.size call_{i}, .-call_{i}',
                  f'.type return_stub_{i},@function', f'return_stub_{i}:',
                  f'    mov ${i + 1}, %al',
                  '    .byte 0xe9', '    .long dispatch_resume - (. + 4)',
                  f'.size return_stub_{i}, .-return_stub_{i}']
    lines += ['    .org caller_bank + 1024, 0x90',
              '.global shared_callee', '.type shared_callee,@function', 'shared_callee:',
              '    mov $0, %al', '.global ret_site', 'ret_site:', '    ret',
              '.size shared_callee, .-shared_callee']
    return '\n'.join(lines)


def pc_record(i, symbol, syms):
    pc = syms[symbol]
    return {'id': i, 'symbol': symbol, 'pc': pc, 'pc_hex': hex(pc), 'low6': pc & 63,
            'svm_site_index': (pc >> 2) & 15, 'fex_l1_index': pc & 0xfffff}


def population(rows, field):
    return dict(sorted(Counter(row[field] for row in rows).items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--iterations', type=lambda s: int(s, 0), default=1 << 22)
    parser.add_argument('--clang', type=Path, default=base.LLVM / 'clang')
    parser.add_argument('--ld', type=Path, default=Path(shutil.which('ld.lld') or base.NDK / 'ld.lld'))
    parser.add_argument('--nm', type=Path, default=base.LLVM / 'llvm-nm')
    args = parser.parse_args()
    if not 0 < args.iterations < 1 << 63:
        parser.error('--iterations must be in 1..2^63-1')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    scratch = out / 'tmp'
    scratch.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(scratch))
    template_path = base.HERE / 'guest.S.in'
    template = template_path.read_text()
    selectors = [i % 4 for i in range(base.TABLE_SIZE)]
    cases = []
    for name, spread in [('return-4-spread', True), ('return-4-collide', False)]:
        source, obj, elf = (out / (name + ext) for ext in ['.S', '.o', '.elf'])
        assembly = (template.replace('@ITERATIONS@', str(args.iterations))
                    .replace('@DISPATCH@', '    jmp *%r11').replace('@TARGETS@', bank(spread))
                    .replace('@TABLE@', '\n'.join(f'    .quad caller_entry_{i}' for i in selectors)))
        source.write_text(assembly)
        commands = [[args.clang, '--target=x86_64-linux-gnu', '-c', '-x', 'assembler', source, '-o', obj],
                    [args.ld, '-m', 'elf_x86_64', '--static', '--entry=_start', '--build-id=sha1',
                     '--image-base=0x400000', '-Ttext=0x401000', '-z', 'max-page-size=4096',
                     '-z', 'noexecstack', '-o', elf, obj]]
        for command in commands:
            base.run(command, env)
        syms = base.symbols(args.nm, elf, env)
        origin = syms['caller_bank']
        entries = [pc_record(i, f'caller_entry_{i}', syms) for i in range(4)]
        returns = [pc_record(i, f'return_stub_{i}', syms) for i in range(4)]
        calls = [pc_record(i, f'call_{i}', syms) for i in range(4)]
        for i in range(4):
            if (entries[i]['pc'] != origin + i * 256 + 4 * i or
                    returns[i]['pc'] != origin + i * 256 + 80 + (4 * i if spread else 0) or
                    calls[i]['pc'] + 5 != returns[i]['pc']):
                raise RuntimeError('assembler changed a caller/return address: ' + name)
            entries[i]['return_value'] = i + 1
            entries[i]['value_producer'] = returns[i]['symbol']
        if syms['shared_callee'] != origin + 1024 or syms['ret_site'] != syms['shared_callee'] + 2:
            raise RuntimeError('shared callee/RET address changed: ' + name)
        addresses = [entry['pc'] for entry in entries]
        expected = base.checksum(selectors, addresses, args.iterations)
        record = {'name': name, 'branch': 'return_fanin', 'targets': 4, 'stride': 260,
                  'bank_stride': 256, 'pattern': 'round_robin', 'direct': False,
                  'iterations': args.iterations, 'warmup_iterations': 4096,
                  'rounds': 2, 'measured_round': 1,
                  'expected_checksum': expected['checksum'], 'checksum_components': expected,
                  'warmup_checksum': base.checksum(selectors, addresses, 4096)['checksum'],
                  'binary': base.identity(elf), 'assembly': base.identity(source),
                  'target_symbols': entries, 'return_symbols': returns, 'call_symbols': calls,
                  'shared_callee_pc': syms['shared_callee'], 'ret_site': syms['ret_site'],
                  'shared_callee_return_value': 0,
                  'svm_site_index_population': population(entries, 'svm_site_index'),
                  'target_low6_population': population(entries, 'low6'),
                  'return_svm_site_index_population': population(returns, 'svm_site_index'),
                  'return_low6_population': population(returns, 'low6'),
                  'sequence': selectors, 'sequence_target_counts': population(
                      [{'id': i} for i in selectors], 'id'),
                  'symbols': {k: syms[k] for k in ['_start', 'kernel', 'selection_loop', 'dispatch_site',
                                                 'dispatch_resume', 'measured_kernel_call',
                                                 'sequence_table', 'caller_bank', 'shared_callee', 'ret_site']},
                  'repeated_kernel_sites': {'indirect_jmp': 1, 'direct_call': 4, 'shared_ret': 1},
                  'commands': [[str(x) for x in command] for command in commands]}
        (out / (name + '.json')).write_text(json.dumps(record, indent=2) + '\n')
        obj.unlink()
        cases.append(record)
    # Same incoming PCs/table and common RET PC: only CALL/return offsets differ.
    for key in ['symbols', 'sequence', 'target_symbols', 'expected_checksum']:
        if cases[0][key] != cases[1][key]:
            raise RuntimeError('paired fixture changed a common input: ' + key)
    manifest = {'kind': 'freestanding_x86_64_return_fanin_pair',
                'iterations': args.iterations, 'warmup_iterations': 4096,
                'table_entries': base.TABLE_SIZE, 'rounds': 2, 'measured_round': 1,
                'protocol': {'ready': 'READY <guest_gettid_decimal>\\n', 'gate': 'G byte on stdin',
                             'done': 'DONE <elapsed_ns_decimal> <checksum_decimal>\\n',
                             'acknowledge': 'X byte on stdin', 'functional_stdin': 'GXGX',
                             'successful_exit': 0, 'io_error_exit': 70,
                             'clock': 'guest CLOCK_MONOTONIC syscall 228',
                             'warmup': '4096 kernel iterations, then two identical-PC protocol rounds; measure only round 1'},
                'layout': {'incoming': 'bank_base+i*256+4*i; low6=0,4,8,12',
                           'spread_returns': 'bank_base+i*256+80+4*i; low6=16,20,24,28',
                           'collide_returns': 'bank_base+i*256+80; low6=16 for all four',
                           'callee': 'bank_base+1024; MOV AL,0; one shared RET at callee+2',
                           'route': 'indirect JMP entry_i; short JMP call_i; direct CALL shared_callee; actual RET return_stub_i; MOV AL,i+1; near JMP dispatch_resume',
                           'padding': 'Only entry short JMP, CALL, callee and return stub execute; bank padding is skipped',
                           'svm_site_index': '(target_pc >> 2) & 15', 'fex_l1_index': 'target_pc & 0xfffff'},
                'checksum': 'u64(sum of return_stub i+1 results) XOR u64(XOR of every loaded caller-entry pointer)',
                'inputs': {'builder': base.identity(Path(__file__)),
                           'frozen_helpers': base.identity(base.HERE / 'build.py'),
                           'frozen_template': base.identity(template_path)},
                'tools': {'clang': base.identity(args.clang),
                          'clang_version': base.run([args.clang, '--version'], env).splitlines()[0],
                          'ld': base.identity(args.ld), 'ld_version': base.run([args.ld, '--version'], env).strip(),
                          'nm': base.identity(args.nm)},
                'qualification': 'Built only. Static symbol placement verified; no guest, phone, PMU or runtime test was run. Original 12-case builder/template/ELFs were not changed.',
                'cases': cases}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    scratch.rmdir()
    print(json.dumps({'out': str(out), 'cases': len(cases), 'iterations': args.iterations,
                      'rounds': 2, 'expected_checksum': cases[0]['expected_checksum'],
                      'manifest': str(out / 'manifest.json')}))


if __name__ == '__main__':
    main()
