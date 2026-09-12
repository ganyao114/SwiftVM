#!/usr/bin/env python3
"""Build the short Linux guest and its independently calculated output."""

import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def expected_output():
    data = bytes.fromhex('ff 80 7f 01 fe 20 c0 04 f0 08 a5 11')
    table = bytearray([0x5a] * 288)
    previous = data[0]
    for index in range(1, 11, 2):
        table[16 + ((data[index] - 8 * previous) & 255)] = index
        table[16 + ((data[index + 1] - 8 * data[index]) & 255)] = index + 1
        previous = data[index + 1]
    a = [i + 0.5 for i in range(32)]
    sums = []
    for _ in range(2):
        c = a.copy()
        b = [3 * value for value in c]
        a = [left + right for left, right in zip(b, c)]
        sums.extend(a)
        a = [left + 3 * right for left, right in zip(b, c)]
    return bytes(table) + struct.pack('<160d', *(a + b + c + sums))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--clang', default=os.environ.get('CLANG', 'clang'))
    parser.add_argument('--objcopy', default=os.environ.get('LLVM_OBJCOPY', 'llvm-objcopy'))
    args = parser.parse_args()
    output = args.out.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError('output directory must be empty')
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name('bounded_loops_x86_64.S')
    with tempfile.TemporaryDirectory() as directory:
        obj = Path(directory) / 'guest.o'
        binary = Path(directory) / 'guest.bin'
        subprocess.run([args.clang, '--target=x86_64-linux-gnu', '-c', str(source),
                        '-o', str(obj)], check=True)
        subprocess.run([args.objcopy, '-O', 'binary', '--only-section=.text',
                        str(obj), str(binary)], check=True)
        code = binary.read_bytes()
    # One small load segment; code and writable data occupy separate pages.
    page = 4096
    base = 0x400000
    size = page + len(code)
    header = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF' + bytes([2, 1, 1]) + bytes(9),
                         2, 62, 1, base + page, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    segment = struct.pack('<IIQQQQQQ', 1, 7, 0, base, base, size, size, page)
    guest = output / 'bounded_loops_x86_64'
    guest.write_bytes((header + segment).ljust(page, b'\0') + code)
    guest.chmod(0o755)
    expected = expected_output()
    (output / 'expected.bin').write_bytes(expected)
    print(f'guest={guest} bytes={size} sha256={hashlib.sha256(guest.read_bytes()).hexdigest()}')
    print(f'expected_bytes={len(expected)} sha256={hashlib.sha256(expected).hexdigest()}')


if __name__ == '__main__':
    main()
