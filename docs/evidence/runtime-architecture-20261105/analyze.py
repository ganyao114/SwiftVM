"""Summarize every fixed sample; never replace, pool, or select rounds."""
from pathlib import Path
import json
import statistics

P = Path('/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/runtime-architecture-20261105')
ARMS = ['baseline-block', 'baseline-function', 'block', 'function', 'fex']
COMPONENTS = ['frontend_ns', 'compile_decode_ns', 'optimize_ns', 'backend_ns',
              'publication_ns', 'code_release_ns', 'compile_ns']
QUALITY = ['compile_attempts', 'function_attempts', 'block_attempts', 'fallbacks',
           'emitted_guest_instructions', 'emitted_guest_bytes', 'ir_nodes',
           'operand_words', 'optimized_erased', 'optimized_uniform_forwarded',
           'optimized_folded', 'spill_slots', 'max_frame_bytes', 'native_bytes',
           'code_units_published', 'peak_cache_entries', 'static_links', 'fault_sites']

def stats(values):
    return {'median': statistics.median(values), 'min': min(values), 'max': max(values), 'samples': values}

def save(name, value):
    (P/name).write_text(json.dumps(value, indent=2)+'\n')

compile_rows = json.loads((P/'compile-records.json').read_text())
assert len(compile_rows) == 200 and all(r['valid'] for r in compile_rows)
names = list(dict.fromkeys(r['case'] for r in compile_rows))
compile_summary, quality, comparisons = [], [], []
for name in names:
    row = {'case': name, 'arms': {}}
    for arm in ARMS:
        records = [r for r in compile_rows if r['case']==name and r['arm']==arm]
        assert sorted(r['round'] for r in records) == list(range(5))
        values = stats([r['compile_excluding_byte_ns']/1e6 for r in records])
        if arm!='fex':
            values['components_ms'] = {k: stats([r['record'][k]/1e6 for r in records]) for k in COMPONENTS}
            values['frontend_excluding_byte_ms'] = stats([(r['record']['frontend_ns']-r['record']['compile_decode_ns'])/1e6 for r in records])
        row['arms'][arm] = values
    for scope in ['block','function']:
        candidate, baseline, fex = (row['arms'][a]['median'] for a in [scope, 'baseline-'+scope, 'fex'])
        comparisons.append({'case':name, 'scope':scope, 'compile_ms':candidate,
                            'baseline_compile_ms':baseline, 'fex_compile_ms':fex,
                            'compile_vs_baseline':candidate/baseline, 'compile_vs_fex':candidate/fex})
        a = [r['record'] for r in compile_rows if r['case']==name and r['arm']==scope]
        b = [r['record'] for r in compile_rows if r['case']==name and r['arm']=='baseline-'+scope]
        quality.append({'case':name, 'scope':scope, 'fields':{
            k:{'baseline':sorted({r[k] for r in b}), 'candidate':sorted({r[k] for r in a}),
               'equal':{r[k] for r in b}=={r[k] for r in a}} for k in QUALITY}})
    compile_summary.append(row)
save('compile-summary.json',compile_summary)
save('quality-comparison.json',quality)

runtime_file = P/'runtime-records.json'
if runtime_file.exists():
    rows=json.loads(runtime_file.read_text())
    assert len(rows)==200 and all(r['valid'] for r in rows)
    runtime_summary=[]
    for name in names:
        row={'case':name,'arms':{}}
        for arm in ARMS:
            records=[r for r in rows if r['case']==name and r['arm']==arm]
            assert sorted(r['round'] for r in records)==list(range(5))
            row['arms'][arm]={'seconds':stats([r['seconds'] for r in records]),
                'whole_child_seconds':stats([r['whole_child_seconds'] for r in records]),
                'pmu':{k:stats([r['pmu'][k] for r in records]) for k in ['cpu-cycles:u','instructions:u','branch-misses:u']}}
        for scope in ['block','function']:
            c=next(c for c in comparisons if c['case']==name and c['scope']==scope)
            a,b,f=(row['arms'][arm] for arm in [scope,'baseline-'+scope,'fex'])
            c.update({'runtime_seconds':a['seconds']['median'],
                'runtime_vs_baseline':a['seconds']['median']/b['seconds']['median'],
                'runtime_vs_fex':a['seconds']['median']/f['seconds']['median'],
                'cycles_vs_baseline':a['pmu']['cpu-cycles:u']['median']/b['pmu']['cpu-cycles:u']['median'],
                'cycles_vs_fex':a['pmu']['cpu-cycles:u']['median']/f['pmu']['cpu-cycles:u']['median'],
                'instructions_vs_baseline':a['pmu']['instructions:u']['median']/b['pmu']['instructions:u']['median'],
                'instructions_vs_fex':a['pmu']['instructions:u']['median']/f['pmu']['instructions:u']['median']})
            c['gates']={'time':c['runtime_vs_fex']<=1.10,'cycles':c['cycles_vs_fex']<=1.10,
                        'compile':c['compile_vs_fex']<=2.0}
        runtime_summary.append(row)
    save('runtime-summary.json',runtime_summary)
save('comparisons.json',comparisons)
for c in comparisons:
    print(c['case'],c['scope'],f"compile {c['compile_ms']:.4f} ms; old {c['compile_vs_baseline']:.4f}x; FEX {c['compile_vs_fex']:.4f}x",
          c.get('gates','runtime pending'))
