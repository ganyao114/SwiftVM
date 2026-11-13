from pathlib import Path
import hashlib,json,os,platform,shutil,statistics,subprocess,sys,time
ROOT=Path('/mnt/d/Dev/SwiftVM');W=Path('/home/proton/.cache/SwiftVM/dynarmic-rebench-20261112-1756');BASE=Path('/home/proton/.cache/SwiftVM')
sys.path.insert(0,str(ROOT/'tools'));import bench_dynarmic as bench
os.sched_setaffinity(0,{2})
W.joinpath('runners').mkdir(exist_ok=True)
for name,source in [('swiftvm',BASE/'arm64-x64-residual-impl-20261112/final_compare'),('dynarmic',BASE/'dynarmic-bench-20261111/runners/dynarmic_arm64_compare_bench')]:
 shutil.copy2(source,W/'runners'/name)
manifest=json.loads((ROOT/'docs/evidence/arm64-x64-residual-20261112/implementation/manifest.json').read_text())
assert bench.digest(W/'runners/swiftvm')==manifest['provenance']['runners']['final_compare']['sha256']
assert bench.digest(W/'runners/dynarmic')==manifest['provenance']['runners']['dynarmic_compare']['sha256']
guests={name:row['guest'] for name,row in json.loads((BASE/'dynarmic-bench-20261111/final/summary.json').read_text())['workloads'].items()}
guests.update(json.loads((BASE/'dynarmic-gap-20261111/guests.json').read_text()))
guests.update(json.loads((BASE/'arm64-x64-residual-impl-20261112/guests/guests.json').read_text()))
names=['integer','memory','shift','fp','compute','fp-u8','compute-matrix','compute-matrix-scalar','compute-sha','compute-sort','cond','cmpsep','fpmix','compute-fma','matrix-fma']
dyn_source=BASE/'dynarmic-bench-20261111/dynarmic'
commit=subprocess.check_output(['git','-C',str(dyn_source),'rev-parse','HEAD'],text=True).strip()
assert commit=='9d4582339990d4eae53f1dc7160686920fc2075c'
assert not subprocess.check_output(['git','-C',str(dyn_source),'status','--porcelain'],text=True)
qemu=BASE/'wsl-arm-x64-20261111/qemu-packages/root/usr/bin/qemu-aarch64'
result={'status':'running','start_unix':time.time(),'date_local':'2026-11-12','affinity':[2],'samples_per_engine':31,'warm_repeats_per_process':3,'swiftvm_scope':'function','pin_slots':0,'dynarmic_commit':commit,'runners':{name:{'path':str(W/'runners'/name),'sha256':bench.digest(W/'runners'/name)} for name in ['swiftvm','dynarmic']},'code_build_id':manifest['provenance']['code_build_id_header'],'build_reused_after_failure':True,'source_verification':json.loads((W/'source-verification.json').read_text()),'qemu':{'path':str(qemu),'sha256':bench.digest(qemu)},'environment':{'platform':platform.platform(),'cpu':next(x.split(':',1)[1].strip() for x in Path('/proc/cpuinfo').read_text().splitlines() if x.startswith('model name')),'loadavg_start':Path('/proc/loadavg').read_text().strip()},'protocol':{'order':'AB then BA on alternating pairs, 16 AB and 15 BA per workload','warm':'median of 3 repeated calls per new process, then median across processes','cold':'first call including lazy JIT compilation and guest execution','memory':'identical ELF bytes and direct guest/host addresses, reset and full output check outside timer','features':'Dynarmic safe optimizations, unsafe off, normal block linking, cycle counting off','cache':'runner verifies no new compilation during warm calls','source':'validated reconstruction build, matched archive SHA-256; rebuild failure and unreadable paths recorded'},'workloads':{}}
def save():
 temp=W/'summary.next';temp.write_text(json.dumps(result,indent=2)+'\n');temp.replace(W/'summary.json')
save()
with (W/'runs.jsonl').open('w') as raw:
 for name in names:
  guest=guests[name];assert bench.digest(Path(guest['path']))==guest['sha256'],name
  reference=subprocess.run([str(qemu),guest['path']],capture_output=True,check=True,timeout=60)
  assert reference.stdout==bytes.fromhex(guest['expected_hex']),name
  config={'swiftvm':{'engine':'swiftvm','scope':'function','pins':0},'dynarmic':{'engine':'dynarmic','scope':'block','pins':0}}
  smoke={key:bench.launch(W/'runners'/key,config[key],guest,3) for key in config}
  rows={key:[] for key in config}
  for sample in range(31):
   order=['swiftvm','dynarmic'] if sample%2==0 else ['dynarmic','swiftvm']
   for key in order:
    row=bench.launch(W/'runners'/key,config[key],guest,3);rows[key].append(row)
    raw.write(json.dumps({'workload':name,'pair':sample,'engine':key,'order':order,'measurement':row})+'\n');raw.flush()
  medians={key:{field:statistics.median(row[field] for row in values) for field in ['warm_median_ns','cold_run_ns','initialization_ns','cold_extra_estimate_ns']} for key,values in rows.items()}
  ratios=[a['warm_median_ns']/b['warm_median_ns'] for a,b in zip(rows['swiftvm'],rows['dynarmic'])]
  result['workloads'][name]={'guest':guest,'qemu_verified':True,'smoke':smoke,'measurements':rows,'medians_ns':medians,'swiftvm_over_dynarmic':bench.distribution(ratios),'swiftvm_faster_pairs':sum(x<1 for x in ratios)}
  save();print(name,{key:round(x['warm_median_ns']/1e6,6) for key,x in medians.items()},'ratio',round(statistics.median(ratios),4),flush=True)
result['status']='complete';result['end_unix']=time.time();result['environment']['loadavg_end']=Path('/proc/loadavg').read_text().strip()
for name,row in result['runners'].items():assert bench.digest(Path(row['path']))==row['sha256'],name
save();print('completed 15 workloads, 930 paired measured processes',flush=True)
