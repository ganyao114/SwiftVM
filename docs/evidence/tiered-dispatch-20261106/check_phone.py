from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,subprocess
P=Path(__file__).resolve().parent
R=Path('/Volumes/Data2/SwiftVM-submit-20261104')
A=Path('/Volumes/Data2/swiftvm-middle-build-cache/reconstruction/submit-android')
ADB=['/opt/homebrew/bin/adb','-s','bf1359ff']
NS='/data/local/tmp/swiftvm-tiered-dispatch-20261106'
TESTS=['svm_ir_direct_test','svm_ir_optimize_test','svm_arm64_compile_test','svm_compile_pipeline_test','svm_instruction_coverage_test','svm_avx_coverage_test','svm_x87_coverage_test','svm_runtime_engine_test','svm_x86_interpreter_test']
commands=[]
results=[]
prepared=False

def save(name,value): (P/name).write_text(json.dumps(value,indent=2)+'\n')
def run(label,args,timeout=100):
    argv=ADB+args
    result=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
    (P/(label+'.stdout')).write_text(result.stdout)
    (P/(label+'.stderr')).write_text(result.stderr)
    commands.append({'label':label,'argv':argv,'exit':result.returncode})
    save('commands.json',commands)
    if result.returncode: raise RuntimeError(label+': '+result.stderr[-1200:])
    return result

def shell(label,command,timeout=100): return run(label,['shell',command],timeout)

sources=sorted(path for root in ['source/runtime','source/launcher','source/tests/rebuild','benchmarks'] for path in (R/root).rglob('*') if path.suffix in {'.cpp','.h','.inc','.S'} and 'externals' not in path.parts)
save('source-manifest.json',{str(path.relative_to(R)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources})
save('inputs.json',{name:{'path':str(A/name),'bytes':(A/name).stat().st_size,'sha256':hashlib.sha256((A/name).read_bytes()).hexdigest()} for name in TESTS})
try:
    shell('namespace-absent','test ! -e '+NS)
    shell('prepare','mkdir '+NS)
    prepared=True
    for name in TESTS: run('push-'+name,['push',str(A/name),NS+'/'+name])
    shell('permissions','chmod 755 '+NS+'/*')
    for name in TESTS:
        result=shell('phone-'+name,'taskset 10 timeout -k 2 90 '+NS+'/'+name)
        results.append({'case':name,'exit':result.returncode,'valid':True})
        save('phone-tests.json',results)
        print(name+': passed',flush=True)
finally:
    if prepared:
        ps=shell('cleanup-ps','ps -A -o PID,PPID,ARGS').stdout
        active=[line for line in ps.splitlines() if NS in line]
        opened=shell('cleanup-lsof','lsof').stdout
        own_open=[line for line in opened.splitlines() if NS in line]
        receipt={'namespace':NS,'active_processes':active,'open_files':own_open,'remaining_bytes':None,'errors':[]}
        if active or own_open:
            receipt['errors'].append('Owned namespace still active; retained')
        else:
            used=shell('cleanup-du-before','du -sk '+NS).stdout.split()
            receipt['allocated_bytes_removed']=int(used[0])*1024
            before=shell('cleanup-df-before','df -k /data/local/tmp').stdout
            shell('cleanup-remove','rm -rf '+NS)
            shell('cleanup-absent','test ! -e '+NS)
            after=shell('cleanup-df-after','df -k /data/local/tmp').stdout
            receipt['df_actual_free_delta_bytes']=(int(after.splitlines()[-1].split()[3])-int(before.splitlines()[-1].split()[3]))*1024
            receipt['remaining_bytes']=0
        receipt['completed_at_utc']=datetime.now(timezone.utc).isoformat()
        save('phone-cleanup-receipt.json',receipt)
