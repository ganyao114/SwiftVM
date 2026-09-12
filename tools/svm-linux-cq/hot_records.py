"""Read per-code entry counts without mixing the sizes of distinct versions."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re


FIELDS = re.compile(r"([a-z_]+)=(0x[0-9a-fA-F]+|[0-9]+)(?=\s|$)")


@dataclass(frozen=True)
class CodeRecord:
    entries: int
    host_bytes: int
    host_static: int
    move_static: int = 0
    spill_static: int = 0

    def weighted_host(self) -> int:
        return self.entries * self.host_static


@dataclass(frozen=True)
class HotUnit:
    pc: int
    codes: tuple[CodeRecord, ...]

    @property
    def versions(self) -> int:
        return len(self.codes)

    @property
    def entries(self) -> int:
        return sum(code.entries for code in self.codes)

    @property
    def host_static(self) -> int:
        return sum(code.host_static for code in self.codes)

    def weighted_host(self) -> int:
        return sum(code.weighted_host() for code in self.codes)


def read_fields(line: str, tag: str, required: tuple[str, ...]) -> dict[str, int]:
    payload = line.split(tag, 1)[1].strip()
    fields = {}
    for item in payload.split():
        match = FIELDS.fullmatch(item)
        if not match or match[1] in fields:
            raise ValueError(f"invalid or repeated field in {tag}")
        fields[match[1]] = int(match[2], 16 if match[2].startswith('0x') else 10)
    if any(name not in fields for name in required):
        raise ValueError(f"incomplete {tag} record")
    return fields


def code_record(fields: dict[str, int]) -> CodeRecord:
    result = CodeRecord(*(fields[name] for name in ('entries', 'host_bytes', 'host_static')),
                        fields.get('move_static', 0), fields.get('spill_static', 0))
    if result.host_bytes % 4 or result.host_static * 4 > result.host_bytes:
        raise ValueError("host instruction count does not fit the code bytes")
    if result.entries and not result.host_static:
        raise ValueError("executed code has no host instructions")
    return result


def load_hot(path: str | Path) -> dict[int, HotUnit]:
    aggregates = {}
    codes: dict[int, list[CodeRecord]] = {}
    code_ids = set()
    footer = None
    with Path(path).open(encoding='utf-8', errors='strict') as handle:
        for number, line in enumerate(handle, 1):
            tag = next((tag for tag in ('[svm-hot-code]', '[svm-hot-all]', '[svm-hot-end]')
                        if tag in line), None)
            if tag is None:
                continue
            try:
                if footer is not None:
                    raise ValueError('records after the completed capture')
                if tag == '[svm-hot-end]':
                    footer = read_fields(line, tag, ('codes', 'pcs', 'overflow'))
                    continue
                required = ('pc', 'entries', 'host_bytes', 'host_static',
                            'code' if tag == '[svm-hot-code]' else 'versions')
                fields = read_fields(line, tag, required)
                record = code_record(fields)
                pc = fields['pc']
                if tag == '[svm-hot-code]':
                    key = fields['code']
                    if key in code_ids:
                        raise ValueError('duplicate code number')
                    code_ids.add(key)
                    codes.setdefault(pc, []).append(record)
                else:
                    if pc in aggregates or not fields['versions']:
                        raise ValueError('duplicate PC or zero code versions')
                    aggregates[pc] = (fields['versions'], record)
            except ValueError as exc:
                raise ValueError(f'{path}:{number}: {exc}') from exc
    if not aggregates:
        raise ValueError(f'no complete hot-unit records in {path}')
    if codes or footer is not None:
        if footer is None:
            raise ValueError('per-code capture is missing its final record')
        if footer['overflow']:
            raise ValueError('hot counters overflowed; the capture is incomplete')
        if (footer['codes'] != len(code_ids) or
                any(number >= len(code_ids) for number in code_ids) or
                footer['pcs'] != len(aggregates)
                or set(codes) != set(aggregates)):
            raise ValueError('per-code capture has missing units or versions')
        for pc, (versions, aggregate) in aggregates.items():
            rows = codes[pc]
            if (versions != len(rows) or aggregate.entries != sum(r.entries for r in rows)
                    or aggregate.host_static != max(r.host_static for r in rows)
                    or aggregate.host_bytes != max(r.host_bytes for r in rows)):
                raise ValueError(f'per-code counts disagree with the total for {pc:#x}')
    else:
        for pc, (versions, aggregate) in aggregates.items():
            if versions != 1:
                raise ValueError(f'{pc:#x} combines {versions} versions without per-code counts')
            codes[pc] = [aggregate]
    return {pc: HotUnit(pc, tuple(rows)) for pc, rows in codes.items()}
