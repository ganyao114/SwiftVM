#!/usr/bin/env python3
"""Extract exact .type/.size ranges from assembled arm32_user_helpers.S.

Assemble with clang --target=armv7-linux-gnueabi -c, then pass that ELF32
object here. Only source-owned function/object bytes are installed; padding
and fixed instruction counts do not become a second source of truth.
"""
import argparse
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("object", type=Path)
parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[2] /
                    "source/runtime/platform/linux/arm32_user_helpers.inc")
args = parser.parse_args()
data = args.object.read_bytes()
assert data[:7] == b"\x7fELF\x01\x01\x01"
header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
sections = [struct.unpack_from("<IIIIIIIIII", data, header[6] + n * header[11])
            for n in range(header[12])]
records = []
for table in sections:
    if table[1] != 2:
        continue
    strings = sections[table[6]]
    for offset in range(table[4], table[4] + table[5], table[9]):
        name, value, size, info, other, section = struct.unpack_from("<IIIBBH", data, offset)
        if not size or info & 15 not in (1, 2):
            continue
        start = strings[4] + name
        label = data[start:data.index(0, start)].decode()
        source = sections[section]
        assert size % 4 == 0 and value + size <= source[5]
        words = struct.unpack_from("<" + "I" * (size // 4), data, source[4] + value)
        records.append((value, label, words))
records.sort()
assert len(records) == 7
out = ["// Generated from arm32_user_helpers.S by tools/arm32/generate_user_helpers.py."]
for offset, name, words in records:
    out.append("static constexpr std::uint32_t " + name + "[]{" +
               ", ".join(f"0x{word:08x}u" for word in words) + "};")
out.append("static constexpr Code code[]{")
for offset, name, words in records:
    out.append(f"    {{0x{offset:x}, {name}}},")
out.append("};")
encoded = "\n".join(out) + "\n"
if not args.output.exists() or args.output.read_text() != encoded:
    args.output.write_text(encoded)
