from pathlib import Path
import json, math
p=Path(__file__).resolve().parent/'attempt3'
s=json.loads((p/'summary.json').read_text())
old=json.loads(Path('/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/compile-owner-optimization-20261105/buffers-publish/summary.json').read_text())
limits={'seconds':1.1,'cycles':1.1,'compile':2.0}
failures=[{'case':c,'arm':a,'metric':k,'ratio':v,'limit':limits[k]} for c,aa in s['ratios'].items() for a,row in aa.items() for k,v in row.items() if k in limits and v>limits[k]]
s['acceptance']={'limits':limits,'passed':not failures,'failures':failures,'n':3,'runtime_valid':72,'compile_valid':72,'time_kind':'CoreMark guest loop; others whole-child elapsed, no cross-workload average'}
s['worst_ratios']={k:max(row[k] for aa in s['ratios'].values() for row in aa.values()) for k in limits}
rows=json.loads((p/'compile-records.json').read_text())
groups={'formation_and_ir':['raw_decode','builder','generate_ir'],'ir_optimization':['pass_manager','x87','dfe','validation','other_pass'],'allocation_and_encoding':['ra','backend','emit'],'transfer_and_publication':['tail_copy_cache','publish'],'logical_work_buffer_release':['logical_release'],'other':['compile_block','compile_code']}
result={};comparison={}
for case in s['compile']:
 result[case]={};comparison[case]={}
 for arm in ('block','function','fex'):
  x=sorted([r for r in rows if r['arm']==arm and r['case']==case],key=lambda r:r['compile_excluding_byte_ns'])[1];rec=x['record'];total=x['compile_excluding_byte_ns']/1e6
  if arm=='fex':
   st={y['name']:y for y in rec['stages']};assert sum(y['remaining_ticks'] for y in st.values())==rec['root_ticks']
   part={k:sum(st[n]['remaining_ticks'] for n in names)*1000/rec['cntfrq_hz'] for k,names in groups.items()}
  else:
   part={'formation_and_ir':(rec['frontend_ns']-rec['compile_decode_ns'])/1e6,'ir_optimization':rec['optimize_ns']/1e6,'allocation_and_encoding':rec['backend_ns']/1e6,'transfer_and_publication':rec['publication_ns']/1e6,'native_owner_release':rec['code_release_ns']/1e6}
   part['other']=total-sum(part.values());assert part['other']>=0
   comparison[case][arm]={'before_compile_ms':old['compile'][case][arm]['ms'],'after_compile_ms':s['compile'][case][arm]['ms'],'change_fraction':s['compile'][case][arm]['ms']/old['compile'][case][arm]['ms']-1,'work_unchanged':old['compile'][case][arm]['work']==s['compile'][case][arm]['work'],'before_work':old['compile'][case][arm]['work'],'after_work':s['compile'][case][arm]['work']}
  assert math.isclose(sum(part.values()),total,abs_tol=1e-8)
  result[case][arm]={'round':x['round'],'prefix':x['prefix'],'total_ms':total,'exclusive_ms':part}
(p/'compile-breakdown.json').write_text(json.dumps({'method':'Each arm/case uses observation whose total is median. FEX remaining_ticks are exclusive and close exactly; SVM other is measured residual. Native owner release differs from FEX logical working-buffer release.','cases':result},indent=2)+'\n')
(p/'comparison.json').write_text(json.dumps(comparison,indent=2)+'\n')
s['compile_breakdown']='compile-breakdown.json';s['comparison']='comparison.json';s['function_audit']='../audit-accepted-candidate/analysis.json'
s['supplementary_x87_confirmation']={'plan':'../x87-paired-confirmation/plan.json','records':'../x87-paired-confirmation/records.json','summary':'../x87-paired-confirmation/summary.json','separate_from_original_3_runs':True,'changes_original_suite_acceptance':False}
s['status']='reuse_implemented_original_suite_has_one_compile_gate_failure' if failures else 'all_original_gates_passed'
(p/'summary.json').write_text(json.dumps(s,indent=2)+'\n')
print('original fixed-suite gate result',s['acceptance']['passed'],'failures',failures,'worst',s['worst_ratios'])
