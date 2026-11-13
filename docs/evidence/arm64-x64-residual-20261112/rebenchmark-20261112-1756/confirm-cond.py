from pathlib import Path
import json,os,statistics,sys,time
r=Path('/mnt/d/Dev/SwiftVM');w=Path('/home/proton/.cache/SwiftVM/dynarmic-rebench-20261112-1756');sys.path.insert(0,str(r/'tools'));import bench_dynarmic as b
os.sched_setaffinity(0,{2});base=json.loads((w/'summary.json').read_text());guest=base['workloads']['cond']['guest'];rows={k:[] for k in ['swiftvm','dynarmic']}
for sample in range(31):
 for key in (['swiftvm','dynarmic'] if sample%2==0 else ['dynarmic','swiftvm']):
  config={'engine':key,'scope':'function' if key=='swiftvm' else 'block','pins':0}
  rows[key].append(b.launch(w/'runners'/key,config,guest,9))
result={'workload':'cond','samples_per_engine':31,'warm_repeats_per_process':9,'order':'alternating AB and BA','affinity':[2],'runners':base['runners'],'guest':guest,'measurements':rows,'medians_ns':{k:statistics.median(x['warm_median_ns'] for x in v) for k,v in rows.items()},'swiftvm_over_dynarmic':b.distribution([a['warm_median_ns']/c['warm_median_ns'] for a,c in zip(rows['swiftvm'],rows['dynarmic'])])}
(w/'confirmation-cond.json').write_text(json.dumps(result,indent=2)+'\n');print({k:v for k,v in result.items() if k in ['medians_ns','swiftvm_over_dynarmic']},flush=True)
