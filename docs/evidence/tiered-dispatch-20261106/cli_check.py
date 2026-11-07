from pathlib import Path
import hashlib,json,struct,subprocess,shlex
P=Path(__file__).resolve().parent
ADB=['/opt/homebrew/bin/adb','-s','bf1359ff']
NS='/data/local/tmp/swiftvm-tiered-cli-20261106'
runner=Path('/Volumes/Data2/swiftvm-middle-build-cache/reconstruction/submit-android/svm_rebuild_linux')
code=bytes.fromhex('b92800000031ff83c701ffc975f9b83c0000000f05')
header=struct.pack('<16sHHIQQQIHHHHHH',b'\x7fELF\x02\x01\x01'+b'\x00'*9,2,62,1,0x400078,64,0,0,64,56,1,0,0,0)
phdr=struct.pack('<IIQQQQQQ',1,5,0,0x400000,0x400000,120+len(code),120+len(code),4096)
guest=P/'tier-loop.elf'
guest.write_bytes(header+phdr+code)
commands=[]
def call(label,args,expect=0):
    argv=ADB+args
    r=subprocess.run(argv,capture_output=True,text=True,timeout=60)
    (P/(label+'.stdout')).write_text(r.stdout)
    (P/(label+'.stderr')).write_text(r.stderr)
    commands.append({'label':label,'argv':argv,'exit':r.returncode})
    (P/'cli-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    if r.returncode!=expect: raise RuntimeError(label+': '+r.stderr[-2000:])
    return r
prepared=False
try:
    call('cli-absent',['shell','test ! -e '+NS])
    call('cli-prepare',['shell','mkdir '+NS]); prepared=True
    call('cli-push-runner',['push',str(runner),NS+'/svm'])
    call('cli-push-guest',['push',str(guest),NS+'/guest'])
    call('cli-chmod',['shell','chmod 755 '+NS+'/svm'])
    rows=[]
    for scope,threshold in [('block',255),('block',8),('function',8)]:
        label='cli-'+scope+'-'+str(threshold)
        argv=['taskset','10','timeout','-k','2','30',NS+'/svm','--engine','mixed','--scope',scope,'--hot-threshold',str(threshold),NS+'/guest']
        r=call(label,['shell',shlex.join(argv)],40)
        data=json.loads(next(line for line in r.stderr.splitlines() if line.startswith('{')))
        assert data['runtime_completed'] and data['exit_code']==40 and data['engine']=='mixed' and data['hot_threshold']==threshold
        if threshold==255: assert data['compile_attempts']==0 and data['interpreted_steps']==124 and data['native_bytes']==0
        else: assert data['compile_attempts']==1 and 0<data['interpreted_steps']<124 and data['native_bytes']>0
        rows.append(data)
        print(json.dumps({k:data[k] for k in ['scope','hot_threshold','compile_attempts','interpreted_steps','native_bytes']}),flush=True)
    (P/'cli-results.json').write_text(json.dumps(rows,indent=2)+'\n')
    (P/'cli-inputs.json').write_text(json.dumps({str(path):{'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for path in [runner,guest]},indent=2)+'\n')
finally:
    if prepared:
        active=[s for s in call('cli-cleanup-ps',['shell','ps -A -o PID,PPID,ARGS']).stdout.splitlines() if NS in s]
        opened=[s for s in call('cli-cleanup-lsof',['shell','lsof']).stdout.splitlines() if NS in s]
        receipt={'namespace':NS,'active_processes':active,'open_files':opened,'remaining_bytes':None,'errors':[]}
        if active or opened: receipt['errors'].append('Owned namespace still active; retained')
        else:
            before=int(call('cli-cleanup-du',['shell','du -sk '+NS]).stdout.split()[0])*1024
            df_before=int(call('cli-cleanup-df-before',['shell','df -k /data/local/tmp']).stdout.splitlines()[-1].split()[3])*1024
            call('cli-cleanup-remove',['shell','rm -rf '+NS])
            call('cli-cleanup-absent',['shell','test ! -e '+NS])
            df_after=int(call('cli-cleanup-df-after',['shell','df -k /data/local/tmp']).stdout.splitlines()[-1].split()[3])*1024
            receipt.update(allocated_bytes_removed=before,df_actual_free_delta_bytes=df_after-df_before,remaining_bytes=0)
        (P/'cli-cleanup-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
