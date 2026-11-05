#!/usr/bin/env python3
"""Classify observed cold-entry roles against earlier published guest instructions."""
from collections import defaultdict, Counter
import hashlib
import json
from pathlib import Path
import struct

import capstone
from capstone.x86_const import X86_OP_IMM

OUT = Path(__file__).resolve().parent
AUDIT = OUT.parent.parent / 'compile-owner-optimization-20261105/function-audit'
audit = json.loads((AUDIT / 'analysis.json').read_text())
elf_path = Path(audit['inputs']['coremark'])
elf = elf_path.read_bytes()
assert hashlib.sha256(elf).hexdigest() == audit['inputs']['coremark_sha256']
header = struct.unpack_from('<16sHHIQQQIHHHHHH', elf)
loads = [struct.unpack_from('<IIQQQQQQ', elf, header[5] + i * header[9]) for i in range(header[10])]
loads = [v for v in loads if v[0] == 1]
sections = [struct.unpack_from('<IIQQQQIIQQ', elf, header[6] + i * header[11]) for i in range(header[12])]
symbols = []
for s in sections:
    if s[1] != 2:
        continue
    st = sections[s[6]]
    names = elf[st[4]:st[4] + st[5]]
    for at in range(s[4], s[4] + s[5], s[9]):
        name, info, other, index, pc, size = struct.unpack_from('<IBBHQQ', elf, at)
        if info & 15 == 2 and index and size:
            symbols.append((pc, pc + size, names[name:names.find(b'\0', name)].decode(errors='replace')))


def symbol(pc):
    found = [s for s in symbols if s[0] <= pc < s[1]]
    return min(found, key=lambda s: s[1] - s[0])[2] if found else None


def memory(pc, end):
    for _, _, offset, address, physical, filesz, memsz, align in loads:
        if address <= pc <= end <= address + memsz:
            first, last = min(pc - address, filesz), min(end - address, filesz)
            return elf[offset + first:offset + last] + bytes(end - pc - last + first)
    raise RuntimeError(f'outside ELF load: {pc:x}..{end:x}')


decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
decoder.detail = True
decoded = {}


def block_ops(row):
    key = row['pc'], row['end']
    if key not in decoded:
        decoded[key] = list(decoder.disasm(memory(*key), key[0]))
    ops = decoded[key]
    assert len(ops) == row['count']
    assert (ops[-1].address + ops[-1].size if ops else key[0]) == key[1]
    return ops


requests = []
for line in (AUDIT / 'coremark20-function.stderr').read_text().splitlines():
    if not line.startswith('{'):
        continue
    event = json.loads(line)
    kind = event.get('function_audit')
    if not kind:
        continue
    if kind == 'begin':
        requests.append({'begin': event, 'blocks': []})
    elif kind == 'block':
        requests[-1]['blocks'].append(event)
    else:
        requests[-1][kind] = event

coverage = {r['seq']: r for r in map(json.loads, (AUDIT / 'unit-coverage.jsonl').read_text().splitlines())}
calls_after = defaultdict(dict)
direct_calls = defaultdict(dict)
external_edges = defaultdict(dict)
inside_calls = defaultdict(dict)
indirect_jumps = {}
prior_source_blocks = defaultdict(dict)
result_rows = []
totals = defaultdict(Counter)
vfprintf = []


def proof(op, request, kind, target):
    return {'kind': kind, 'source_pc': op.address if op else None,
            'source_pc_hex': hex(op.address) if op else None, 'target': target,
            'first_owner_seq': request['begin']['seq'], 'first_owner_entry_hex': hex(request['begin']['pc']),
            'source_symbol': symbol(op.address) if op else None,
            'bytes': op.bytes.hex() if op else None,
            'instruction': (op.mnemonic + ' ' + op.op_str).strip() if op else None}


for request in requests:
    begin = request['begin']
    seq, pc, mode = begin['seq'], begin['pc'], begin['fp_mode']
    continuation = list(calls_after[(mode, pc)].values())
    direct = list(direct_calls[(mode, pc)].values())
    edges = list(external_edges[(mode, pc)].values())
    retry = begin['scope'] == 'block' and seq > 1 and not requests[seq - 2]['front']['ok'] and requests[seq - 2]['begin']['pc'] == pc
    if seq == 1:
        assert pc == header[4]
        category = 'initial_elf_root'
    elif retry:
        category = 'same_request_block_retry'
    elif continuation:
        category = 'prior_call_continuation_with_other_direct_role' if direct or edges else 'prior_call_continuation_only_known_direct_role'
    elif direct:
        category = 'prior_direct_call_target'
    elif edges:
        category = 'prior_external_branch_or_frontier'
    else:
        category = 'indirect_or_unobserved_transfer'
    row = {'seq': seq, 'entry_pc': pc, 'entry_hex': hex(pc), 'fp_mode': mode, 'scope': begin['scope'],
           'symbol': symbol(pc), 'category': category, 'published': 'ready' in request,
           'prior_call_continuations': continuation, 'prior_direct_calls': direct, 'prior_external_edges': edges}
    row['prior_nonempty_source_block_at_entry'] = prior_source_blocks.get((mode, pc, False))
    row['prior_empty_source_block_at_entry'] = prior_source_blocks.get((mode, pc, True))
    unit = coverage.get(seq)
    if unit:
        row.update({key: unit[key] for key in ['instructions', 'already_published_instructions', 'new_instructions',
                                              'nodes', 'words', 'entry_in_earlier_unit', 'earlier_owner_seq']})
    else:
        row['failed_prefix_instructions'] = request['front']['guest_instructions']
        row['failed_prefix_nodes'] = request.get('rejected_prefix', {}).get('nodes', 0)
    result_rows.append(row)
    group = totals[category]
    group['attempts'] += 1
    group['published'] += bool(unit)
    group['instructions'] += unit['instructions'] if unit else 0
    group['repeated_instructions'] += unit['already_published_instructions'] if unit else 0
    group['nodes'] += unit['nodes'] if unit else 0
    group['failed_prefix_instructions'] += row.get('failed_prefix_instructions', 0)
    group['failed_prefix_nodes'] += row.get('failed_prefix_nodes', 0)
    if symbol(pc) == '__vfprintf':
        vfprintf.append(row)
    if not unit:
        continue
    blocks = request['blocks']
    if begin['scope'] == 'block':
        blocks = [{'pc': pc, 'end': pc + request['front']['guest_bytes'], 'count': request['front']['guest_instructions']}]
    starts = {b['pc'] for b in blocks}
    for block in blocks:
        if begin['scope'] == 'function':
            prior_source_blocks.setdefault((mode, block['pc'], not bool(block['count'])),
                    {'owner_seq': seq, 'owner_entry_hex': hex(pc), 'pc': block['pc'],
                     'end': block['end'], 'count': block['count']})
        ops = block_ops(block)
        if not ops:
            item = proof(None, request, 'empty_decoded_frontier', block['pc'])
            external_edges[(mode, block['pc'])].setdefault(('empty', seq), item)
            continue
        for op in ops:
            after = op.address + op.size
            if op.group(capstone.CS_GRP_CALL):
                item = proof(op, request, 'call_continuation', after)
                item['call_target_kind'] = 'direct' if op.operands[0].type == X86_OP_IMM else 'indirect'
                item['call_target_hex'] = hex(op.operands[0].imm & ((1 << 64) - 1)) if item['call_target_kind'] == 'direct' else None
                calls_after[(mode, after)].setdefault(op.address, item)
                if item['call_target_kind'] == 'direct':
                    target = op.operands[0].imm & ((1 << 64) - 1)
                    direct_calls[(mode, target)].setdefault(op.address, proof(op, request, 'direct_call_target', target))
        last = ops[-1]
        after = last.address + last.size
        if last.group(capstone.CS_GRP_JUMP):
            targets = []
            if last.operands[0].type == X86_OP_IMM:
                targets.append((last.operands[0].imm & ((1 << 64) - 1), 'direct_branch'))
            else:
                item = proof(last, request, 'indirect_jump', None)
                item['preceding_instructions'] = [(hex(op.address), op.mnemonic, op.op_str) for op in ops[-10:]]
                indirect_jumps.setdefault((mode, last.address), item)
            if last.mnemonic != 'jmp':
                targets.append((after, 'conditional_fallthrough'))
            for target, kind in targets:
                if target not in starts:
                    external_edges[(mode, target)].setdefault(last.address, proof(last, request, kind, target))
        elif not (last.group(capstone.CS_GRP_CALL) or last.group(capstone.CS_GRP_RET)) and after not in starts:
            external_edges[(mode, after)].setdefault(last.address, proof(last, request, 'sequential_or_service_frontier', after))

assert len(result_rows) == 705
assert sum(v['published'] for v in totals.values()) == 700
assert sum(v['instructions'] for v in totals.values()) == 31990
assert sum(v['repeated_instructions'] for v in totals.values()) == 15889
continuation_rows = [r for r in result_rows if r['prior_call_continuations'] and r['published']]
continuation_functions = [r for r in continuation_rows if r['scope'] == 'function']
tables = [
    {'jump_pc': 0x25a7f0, 'base': 0x207790, 'indices': list(range(91)), 'first_owner_seq': 221,
     'bound': 'eax <= 0x5a from CMP@25a7d9 / JA@25a7dc; selector is char - 0x20'},
    {'jump_pc': 0x25c193, 'base': 0x20791c, 'indices': [2, 3], 'first_owner_seq': 314,
     'bound': 'only the two exact literal entries used by observed request PCs are reported; no inferred full table size'},
]
for table in tables:
    table['jump_pc_hex'], table['base_hex'] = hex(table['jump_pc']), hex(table['base'])
    table['encoding'] = 'signed32 table entry + base; MOVSXD / ADD / JMP RAX'
    table['readonly_elf_segment'] = next(bool(not flags & 2) for typ, flags, off, start, phys, fs, ms, al in loads
                                        if start <= table['base'] < start + ms)
    table['targets'] = {index: table['base'] + struct.unpack('<i', memory(table['base'] + index * 4, table['base'] + index * 4 + 4))[0]
                        for index in table['indices']}
for row in result_rows:
    row['prior_vfprintf_jump_tables'] = []
    for table in tables:
        if row['seq'] <= table['first_owner_seq'] or row['symbol'] != '__vfprintf':
            continue
        indices = [index for index, pc in table['targets'].items() if pc == row['entry_pc']]
        if indices:
            row['prior_vfprintf_jump_tables'].append({'jump_pc_hex': table['jump_pc_hex'], 'base_hex': table['base_hex'],
                                                       'indices': indices, 'first_owner_seq': table['first_owner_seq']})
main_sequences = set(audit['top_repeated_ranges'][0]['owner_sequences'])
main14 = [row for row in vfprintf if row['seq'] in main_sequences]
assert len(main14) == 14
prior_b_rows = [r for r in result_rows if r['published'] and r['prior_nonempty_source_block_at_entry']]
entire_prior_rows = [r for r in result_rows if r.get('entry_in_earlier_unit')]
assert len(prior_b_rows) == len(entire_prior_rows) == 18
assert all(r['prior_nonempty_source_block_at_entry'] for r in entire_prior_rows)
table_rows = [r for r in result_rows if r['published'] and r['prior_vfprintf_jump_tables'] and r['category'] == 'indirect_or_unobserved_transfer']
packet = {'kind': 'static_incoming_roles_of_observed_cold_requests_not_dynamic_branch_trace',
          'inputs': {'production_source_digest': audit['inputs']['production_source_digest'],
                     'trace_sha256': hashlib.sha256((AUDIT / 'coremark20-function.stderr').read_bytes()).hexdigest(),
                     'coremark_sha256': hashlib.sha256(elf).hexdigest()},
          'categories': {name: dict(value) for name, value in totals.items()},
          'published_call_continuation_role': {'units': len(continuation_rows),
                                              'instructions': sum(r['instructions'] for r in continuation_rows),
                                              'repeated_instructions': sum(r['already_published_instructions'] for r in continuation_rows),
                                              'nodes': sum(r['nodes'] for r in continuation_rows)},
          'successful_function_call_continuation_role': {'units': len(continuation_functions),
                                                         'repeated_instructions': sum(r['already_published_instructions'] for r in continuation_functions)},
          'call_role_overlap_note': '436 published CALL-after-PC matches include 435 Function successes and one Block retry of a failed continuation request; these role counts are not a disjoint transfer-origin partition.',
          'source_b_entry_matches': {'entirely_repeated_roots': 18, 'at_prior_nonempty_source_b_start': 18,
                                     'at_prior_b_interior_instruction': 0, 'all_published_roots_at_prior_source_b_start': 18,
                                     'additional_nonfullyrepeated_roots_at_prior_source_b_start': 0,
                                     'instructions': sum(r['instructions'] for r in prior_b_rows),
                                     'nodes': sum(r['nodes'] for r in prior_b_rows), 'rows': prior_b_rows},
          'vfprintf_requests': vfprintf,
          'vfprintf_main14': main14,
          'vfprintf_indirect_jumps': [r for r in indirect_jumps.values() if r['source_symbol'] == '__vfprintf'],
          'vfprintf_jump_table_evidence': tables,
          'vfprintf_jump_table_entry_matches': {'units': len(table_rows), 'repeated_instructions': sum(r['already_published_instructions'] for r in table_rows)},
          'limitations': ['Prior compiled CALL and continuation equality is directly observed; dynamic last transfer is not logged.',
                          'Multiple known incoming direct roles are kept explicit. Absence of a direct match is not proof of a particular indirect CALL/JMP/RET.',
                          'Source coverage repetition is not a claim that old internal labels satisfy the entry ABI.',
                          'No hypothetical continuation formation or codegen was simulated.']}
(OUT / 'cause.json').write_text(json.dumps(packet, indent=2) + '\n')
(OUT / 'request-roles.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in result_rows))
print(json.dumps({k: packet[k] for k in ['categories', 'published_call_continuation_role']}, indent=2))
for row in vfprintf:
    print('VF', row['seq'], row['entry_hex'], row['category'], row.get('instructions'), row.get('already_published_instructions'),
          [(p['source_pc_hex'], p['instruction'], p['first_owner_seq']) for p in row['prior_call_continuations']])
for item in packet['vfprintf_indirect_jumps']:
    print('INDIRECT', json.dumps(item))
