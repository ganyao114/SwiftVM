#!/usr/bin/env python3
"""Execute x86-64 case bytes on the host CPU as an independent reference.

The probe is built ad hoc and is intentionally outside the shipped runtime:
it maps the case's memory, loads its architectural GPR/XMM/YMM/flags/MXCSR
state, executes exactly one guest instruction, and reports the result in the
same Observation JSON shape used by svm_correctness compare.
"""
import argparse
import json
import os
import platform
import struct
import subprocess
import time
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HEADER = 704
FLAGS_MASK = 0x8D5


def build_probe(output):
    output = Path(output)
    if output.exists():
        return output
    sources = [str(HERE / 'native_probe.c'), str(HERE / 'native_probe_stub.S')]
    command = ['clang', '-O2']
    if platform.system() == 'Darwin' and platform.machine() == 'arm64':
        command += ['-arch', 'x86_64', '-Wl,-pagezero_size,0x10000']
    command += sources + ['-o', str(output)]
    subprocess.run(command, check=True)
    return output


def inspect_case(dut, case):
    completed = subprocess.run([str(dut), 'inspect', '--case', str(case)],
                               check=True, capture_output=True, text=True)
    return json.loads(completed.stdout)


def export_input(case_json, path):
    initial = case_json['initial']
    code = bytes(case_json['code'])
    if len(code) > 16:
        raise SystemExit('instruction longer than 16 bytes')
    if case_json['terminal_pc'] != case_json['code_address'] + len(code):
        raise SystemExit('case is not a single completed instruction')
    header = bytearray(HEADER)
    header[0:8] = b'SVNPROB1'
    struct.pack_into('<Q', header, 8, case_json['code_address'])
    struct.pack_into('<Q', header, 16, case_json['terminal_pc'])
    struct.pack_into('<I', header, 24, len(code))
    struct.pack_into('<I', header, 28, len(case_json['memory']))
    header[32:32 + len(code)] = code
    gpr = list(initial['gpr'][:16]) + [0] * (16 - min(16, len(initial['gpr'])))
    struct.pack_into('<16Q', header, 48, *gpr)
    struct.pack_into('<Q', header, 176, (initial['flags'] & FLAGS_MASK) | 2)
    struct.pack_into('<I', header, 184, initial['fp_control'])
    for index in range(16):
        vector = bytes(initial['vectors'][index])[:32]
        header[192 + index * 32:192 + index * 32 + len(vector)] = vector
    output = bytearray(header)
    for region in case_json['memory']:
        data = bytes(region['bytes'])
        if len(data) > region['size']:
            raise SystemExit('region size mismatch')
        output += struct.pack('<QQII', region['address'], region['size'],
                              region['protection'], len(data))
        output += data
    Path(path).write_bytes(output)


def load_observation(path, case_json, terminal_pc):
    data = Path(path).read_bytes()
    if data[0:8] != b'SVNOUT01':
        raise SystemExit('bad probe output')
    status = struct.unpack_from('<I', data, 8)[0]
    fault = struct.unpack_from('<Q', data, 16)[0]
    gpr = list(struct.unpack_from('<16Q', data, 24))
    flags = struct.unpack_from('<Q', data, 152)[0]
    mxcsr = struct.unpack_from('<I', data, 160)[0]
    ymm = [list(data[168 + i * 32:168 + (i + 1) * 32]) for i in range(16)]
    initial = case_json['initial']
    vectors = ymm + [list(row) for row in initial['vectors'][16:32]]
    observation = {
        'protocol_version': 1,
        'outcome': 'fault' if status else 'terminal',
        'stage': 'execute',
        'error': '',
        'backend': 'native-x86_64',
        'state': {
            'gpr': gpr + [0] * 16,
            'pc': case_json['code_address'] if status else terminal_pc,
            'flags': flags,
            'vectors': vectors,
            'fp_control': mxcsr,
            'fp_status': 0,
            'tls': list(initial['tls']),
            'x87': [list(row) for row in initial['x87']],
            'x87_control': initial['x87_control'],
            'x87_status': initial['x87_status'],
            'x87_tag': initial['x87_tag'],
            'x87_opcode': initial['x87_opcode'],
            'x87_ip': initial['x87_ip'],
            'x87_dp': initial['x87_dp'],
        },
        'memory': [],
        'fault_kind': 'memory' if status else '',
        'fault_access': 'read' if status else '',
        'fault_pc': case_json['code_address'],
        'fault_address': fault,
        'fault_width': 0,
        'fault_vector': -1,
        'fault_address_valid': bool(status),
        'steps': 0 if status else 1,
    }
    cursor = 688
    count = struct.unpack_from('<I', data, 680)[0]
    for _ in range(count):
        address, size, protection, stored = struct.unpack_from('<QQII', data, cursor)
        cursor += 24
        observation['memory'].append({
            'address': address,
            'size': size,
            'protection': protection,
            'bytes': list(data[cursor:cursor + stored]),
        })
        cursor += stored
    return observation


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dut', type=Path, required=True,
                        help='host svm_correctness used for case decoding')
    parser.add_argument('--case', type=Path, action='append', default=[])
    parser.add_argument('--case-list', type=Path,
                        help='file with one case path per line')
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=10.0)
    args = parser.parse_args()
    cases = list(args.case)
    if args.case_list:
        cases += [Path(line.strip()) for line in args.case_list.read_text().splitlines()
                  if line.strip()]
    if not cases:
        parser.error('at least one --case or --case-list is required')
    args.out_dir.mkdir(parents=True, exist_ok=True)
    probe = build_probe(args.out_dir / 'native_probe')
    results = {}
    for case in cases:
        case_json = inspect_case(args.dut, case)
        if case_json['isa'] != 'x86_64':
            results[case.name] = 'skipped: not x86_64'
            continue
        input_path = args.out_dir / (case.stem + '.probe-in')
        output_path = args.out_dir / (case.stem + '.probe-out')
        export_input(case_json, input_path)
        status = None
        for attempt in range(20):
            try:
                subprocess.run([str(probe), str(input_path), str(output_path)],
                               check=True, timeout=args.timeout,
                               capture_output=True, text=True)
                status = 'ok'
                break
            except subprocess.TimeoutExpired:
                status = 'timeout'
                break
            except subprocess.CalledProcessError as error:
                # Rosetta reserves low translation-cache space on the first
                # run of a freshly built probe; retry after it settles.
                status = f'probe failed: {error.stderr.strip()}'
                time.sleep(0.2)
        if status != 'ok':
            results[case.name] = status
            continue
        observation = load_observation(output_path, case_json, case_json['terminal_pc'])
        observation_path = args.out_dir / (case.stem + '.json')
        observation_path.write_text(json.dumps(observation))
        results[case.name] = observation['outcome']
    print(json.dumps(results, sort_keys=True))


if __name__ == '__main__':
    main()
