from pathlib import Path
import datetime
import hashlib
import json
import statistics
import sys

p = Path(sys.argv[1]).resolve()
read = lambda name: json.loads((p / name).read_text())
median = statistics.median
smoke = read('smoke-summary.json')
assert len(smoke) == 18 and all(r['valid'] for r in smoke), smoke
runtime, compile_rows = read('runtime-records.json'), read('compile-records.json')
cases = [r['name'] for r in read('cases.json')]
assert len(runtime) == len(compile_rows) == len(cases) * 9
result = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'identity': read('identity.json'), 'inputs': read('inputs.json'),
          'runtime': {}, 'compile': {}, 'ratios': {}}
for case in cases:
    result['runtime'][case], result['compile'][case], result['ratios'][case] = {}, {}, {}
    for arm in ['block', 'function', 'fex']:
        rows = [r for r in runtime if r['case'] == case and r['arm'] == arm]
        comp = [r for r in compile_rows if r['case'] == case and r['arm'] == arm]
        assert len(rows) == len(comp) == 3 and all(r['valid'] for r in rows + comp)
        assert {r['round'] for r in rows} == {0, 1, 2} == {r['round'] for r in comp}
        times = [r['seconds'] for r in rows]
        rr = {'n': 3, 'seconds': median(times), 'min_seconds': min(times), 'max_seconds': max(times),
              'whole_child_seconds': median(r['whole_child_seconds'] for r in rows),
              'cycles': median(r['pmu']['cpu-cycles:u'] for r in rows),
              'instructions': median(r['pmu']['instructions:u'] for r in rows),
              'branch_misses': median(r['pmu']['branch-misses:u'] for r in rows),
              'ipc': median(r['pmu']['instructions:u'] / r['pmu']['cpu-cycles:u'] for r in rows)}
        cc = {'n': 3, 'ms': median(r['compile_excluding_byte_ns'] for r in comp) / 1e6,
              'min_ms': min(r['compile_excluding_byte_ns'] for r in comp) / 1e6,
              'max_ms': max(r['compile_excluding_byte_ns'] for r in comp) / 1e6}
        if arm != 'fex':
            cc['stages_ms'] = {key: median(r['record'][key] for r in comp) / 1e6
                               for key in ['compile_ns', 'frontend_ns', 'optimize_ns', 'backend_ns',
                                           'publication_ns', 'code_release_ns', 'compile_decode_ns']}
            cc['work'] = {key: median(r['record'][key] for r in comp)
                          for key in ['compile_attempts', 'function_attempts', 'block_attempts', 'fallbacks',
                                      'code_units_published', 'emitted_guest_instructions', 'native_bytes',
                                      'ir_nodes', 'operand_words', 'spill_slots', 'max_frame_bytes']}
        else:
            assert all(r['record']['qualified'] and r['record']['closed'] for r in comp)
            cc['inclusive_stages_ms'] = {s['name']: median(next(x['ticks'] for x in r['record']['stages']
                                                        if x['name'] == s['name']) * 1000 / r['record']['cntfrq_hz']
                                                         for r in comp) for s in comp[0]['record']['stages']}
        result['runtime'][case][arm], result['compile'][case][arm] = rr, cc
    for arm in ['block', 'function']:
        rr, ref = result['runtime'][case][arm], result['runtime'][case]['fex']
        result['ratios'][case][arm] = {key: rr[key] / ref[key]
                                      for key in ['seconds', 'cycles', 'instructions', 'branch_misses']}
        result['ratios'][case][arm]['compile'] = result['compile'][case][arm]['ms'] / result['compile'][case]['fex']['ms']
manifest = read('source-manifest.json')
repo = Path(manifest['source_worktree'])
changes = [r['path'] for r in manifest['files']
           if hashlib.sha256((repo / r['path']).read_bytes()).hexdigest() != r['sha256']]
assert not changes, changes
result['source_closure'] = {'digest': manifest['digest'], 'files': len(manifest['files']), 'changed': changes}
result['phone_cleanup'] = read('phone-cleanup-receipt.json')
def remote(name):
    return {line.split(maxsplit=1)[1].strip(): line.split()[0]
            for line in (p / name).read_text().splitlines() if line.strip()}
assert remote('remote-identities-before.stdout') == remote('remote-identities-after.stdout')
result['deployed_inputs_unchanged'] = True
(p / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
for case in cases:
    print(case, 'runtime ms', [round(result['runtime'][case][a]['seconds'] * 1000, 3) for a in ['block', 'function', 'fex']],
          'compile ms', [round(result['compile'][case][a]['ms'], 4) for a in ['block', 'function', 'fex']],
          'compile ratios', [round(result['ratios'][case][a]['compile'], 3) for a in ['block', 'function']])
