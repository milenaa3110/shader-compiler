#!/usr/bin/env python3
"""Profile compiler phases and the LLVM tool pipeline under Linux/WSL.
Run: python3 test/script/profile_compiler.py --perf
Outputs: build/compiler-profile/{report.md,results.json,compiler_samples.csv,*.log}
For scalar/packet execution measurements use test/script/bench_packet_pipeline.py.
"""
import argparse,csv,hashlib,json,os,platform,random,re,shlex,shutil,statistics,subprocess,sys,time,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
PHASE=re.compile(r'\[shader-profile\] phase=(\w+) ms=([0-9.]+)')
def stats(v): return {'median':statistics.median(v),'min':min(v),'max':max(v)}
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-dir',type=Path,default=ROOT/'build')
    ap.add_argument('--output',type=Path,default=ROOT/'build/compiler-profile')
    ap.add_argument('--runs',type=int,default=7)
    ap.add_argument('--perf',action='store_true')
    a=ap.parse_args()
    if a.runs<1: ap.error('counts must be positive')
    build,out=a.build_dir.resolve(),a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    commands,cr,checks,errors=[],[],[],[]
    env=os.environ.copy()
    for k in ['SHADER_PROFILE','SHADER_EMIT_PACKET','IRGEN_SPIRV_DUMP_IR','SHADER_PACKET']: env.pop(k,None)
    env.update(OMP_DYNAMIC='FALSE',OMP_PROC_BIND='close',OMP_PLACES='cores')
    result={'environment':{'platform':platform.platform(),'machine':platform.machine(),'logical_cpus':os.cpu_count(),'affinity_cpus':len(os.sched_getaffinity(0)),'settings':{k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items()}},'checks':checks,'errors':errors}
    def run(cmd,source=None,extra_env=None,log=None,timeout=180):
        cmd=[str(c) for c in cmd]
        commands.append({'argv':cmd,'stdin':str(source) if source else None,'extra_env':extra_env or {}})
        start=time.perf_counter()
        if source:
            with Path(source).open('rb') as f: proc=subprocess.run(cmd,stdin=f,capture_output=True,env=env|(extra_env or {}),cwd=ROOT,timeout=timeout)
        else: proc=subprocess.run(cmd,capture_output=True,env=env|(extra_env or {}),cwd=ROOT,timeout=timeout)
        ms=(time.perf_counter()-start)*1000
        stdout,stderr=proc.stdout.decode(errors='replace'),proc.stderr.decode(errors='replace')
        if log: Path(log).write_text(stdout+stderr)
        if proc.returncode: raise RuntimeError(f'{shlex.join(cmd)} exited {proc.returncode}:\n{stderr[-2500:]}')
        return ms,stdout,stderr
    def sample_perf(name, command, comm=None):
        # Flat software sampling works on this WSL kernel. DWARF stack capture
        # can fail with EFAULT; no call-chain claims are made by this report.
        try:
            with tempfile.TemporaryDirectory(prefix='shader-perf-') as tmp:
                data=Path(tmp)/'perf.data'
                run(['perf','record','-e','cpu-clock:u','-F','499','-o',data,'--',*command],log=out/f'{name}.perf-record.log')
                cmd=['perf','report','--stdio','--no-children','-i',data]
                if comm: cmd+=['--comms',comm]
                run(cmd,log=out/f'{name}.perf-report.txt')
                shutil.copyfile(data,out/f'{name}.perf.data')
                result[name+'_perf']='Flat cpu-clock:u samples; native Linux temporary recording; report and data saved.'
        except RuntimeError as ex:
            result[name+'_perf_unavailable']=str(ex)

    try:
        e=result['environment']
        for tool in ['opt-18','llc-18','clang++-18','g++','llvm-as-18','spirv-val']: e[tool]=shutil.which(tool)
        e['git_head']=run(['git','rev-parse','HEAD'])[1].strip(); e['git_status']=run(['git','status','--short'])[1]
        e['cpu']=next((l.split(':',1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines() if l.startswith('model name')),'unknown')
        e['build_settings']=[l for l in (build/'CMakeCache.txt').read_text().splitlines() if re.match(r'^(CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS[^:]*|SHADER_PACKET_WIDTH|DZN_ICD):',l) and '-ADVANCED:' not in l]
        e['llvm_version']=run(['opt-18','--version'])[1].strip()
        irgen,spvgen=build/'riscv/irgen_riscv',build/'spirv/irgen_spirv'
        anim=ROOT/'test/shaders/animations'
        synthetic=out/'stress_1000.src'
        synthetic.write_text('uniform float uTime; in vec2 vUV; out vec4 FragColor;\n@entry @stage(fragment) fn void main() {\nfloat v0 = vUV.x + uTime;\n'+''.join(f'float v{i} = v{i-1} * 1.00001 + vUV.y;\n' for i in range(1,1001))+'FragColor = vec4(v1000, vUV.x, vUV.y, 1.0);\n}\n')
        cases={'mandelbrot':anim/'mandelbrot_fs.src','waves':anim/'waves_fs.src','city':anim/'city_fs.src','stress_1000':synthetic}
        result['compiler_sources']={n:{'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for n,p in cases.items()}
        jobs=[(n,s,t,x) for n,s in cases.items() for t,x in [('riscv',irgen),('spirv',spvgen)]]
        for n,s,t,x in jobs:
            dest=out/f'{n}.{t}.raw'; run([x,dest],source=s); before=dest.read_bytes()
            run([x,dest],source=s,extra_env={'SHADER_PROFILE':'1'})
            if dest.read_bytes()!=before: raise RuntimeError(f'profiling changed output for {n}/{t}')
            run(['llvm-as-18',dest,'-o',os.devnull] if t=='riscv' else ['spirv-val',dest],log=out/f'{n}.{t}.validation.log')
            checks.append(f'{n}/{t}: profiling on/off output identical; validator passed')
        schedule=jobs*a.runs; random.Random(123).shuffle(schedule)
        for n,s,t,x in schedule:
            ms,_,err=run(['/usr/bin/time','-f','[shader-rss] kb=%M',x,out/f'{n}.{t}.raw'],source=s,extra_env={'SHADER_PROFILE':'1'})
            phases={k:float(v) for k,v in PHASE.findall(err)}
            if not phases: raise RuntimeError('No phase timings: rebuild instrumented compiler tools')
            cr.append({'case':n,'target':t,'process_ms':ms,'max_rss_kib':int(re.search(r'\[shader-rss\] kb=(\d+)',err)[1]),**phases})
        result['io_samples']=[]
        with tempfile.TemporaryDirectory(prefix='shader-io-') as tmp:
            for label,destination in [('mounted_output',out/'io_probe.ll'),('linux_temp',Path(tmp)/'io_probe.ll'),('discard',Path(os.devnull))]:
                for i in range(a.runs+1):
                    ms,_,err=run([irgen,destination],source=synthetic,extra_env={'SHADER_PROFILE':'1'})
                    if i: result['io_samples'].append({'destination':label,'process_ms':ms,**{k:float(v) for k,v in PHASE.findall(err)}})
        print('Compiler phase measurements and output checks complete.',flush=True)
        result['pipeline_samples']=[]; result['sincos_calls']={}
        for n in cases:
            raw,opt,final,obj=[out/f'{n}{suffix}' for suffix in ['.riscv.raw','.opt.ll','.final.ll','.rv.o']]
            stages=[('opt_O3',['opt-18','-O3','--enable-unsafe-fp-math','--fp-contract=fast','-time-passes','-S',raw,'-o',opt]),('sincos',['opt-18',f'-load-pass-plugin={build}/llvm/sincos_opt.so','-passes=sincos-opt','-time-passes','-S',opt,'-o',final]),('llc',['llc-18','-O3','--fp-contract=fast','-filetype=obj','-mtriple=riscv64-unknown-linux-gnu','-mattr=+m,+a,+f,+d,+v',final,'-o',obj])]
            for stage,cmd in stages:
                samples=[]
                for r in range(a.runs+1):
                    ms,_,_=run(cmd,log=out/f'{n}.{stage}.log')
                    if r: samples.append(ms)
                result['pipeline_samples'].append({'case':n,'stage':stage,'samples_ms':samples})
            run(['llvm-as-18',final,'-o',os.devnull])
            result['sincos_calls'][n]=len(re.findall(r'\bcall\b[^\n]*@sincosf\(',final.read_text()))
        print('LLVM optimization and RISC-V code-generation timings complete.',flush=True)
        if a.perf:
            loop=out/'perf_compile_loop.py'
            loop.write_text('import subprocess\n'+f'exe={str(irgen)!r}\nsource={str(synthetic)!r}\nout={str(out/"perf.raw.ll")!r}\n'+"for _ in range(40):\n    with open(source,'rb') as f:\n        subprocess.run([exe,out],stdin=f,stdout=subprocess.DEVNULL,check=True)\n")
            sample_perf('compiler',['python3',loop],comm='irgen_riscv')
    except Exception as ex: errors.append(str(ex)); print(str(ex),file=sys.stderr)
    finally:
        result['compiler_samples']=cr
        (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if cr:
            with (out/'compiler_samples.csv').open('w',newline='') as f:
                w=csv.DictWriter(f,fieldnames=sorted(set().union(*(r.keys() for r in cr)))); w.writeheader(); w.writerows(cr)
    lines=['# Shader compiler profiling results','',f'CPU: {result["environment"].get("cpu","unknown")}',f'Platform: {platform.platform()}',f'Commit: {result["environment"].get("git_head","unknown")} (working-tree state in results.json)','',
      'Compiler process time includes startup, dynamic linking, instrumentation and filesystem I/O. Phase timings exclude startup/shutdown and AST destruction. Lexing occurs inside parsing. Peak RSS includes LLVM and other loaded libraries.','']
    lines+=['Build configuration:']+['- `'+setting+'`' for setting in result['environment'].get('build_settings',[])]+['']
    if cr:
        lines+=['## Compiler phases','', '| Shader | Target | Process median ms | Lex/parse | Sema | LLVM IR | Packet / SPIR-V emit | Write | Peak RSS MiB |','|---|---|---:|---:|---:|---:|---:|---:|---:|']
        for n,t in sorted({(r['case'],r['target']) for r in cr}):
            rows=[r for r in cr if (r['case'],r['target'])==(n,t)]
            med=lambda key:statistics.median(r.get(key,0) for r in rows)
            lines.append(f'| {n} | {t} | {med("process_ms"):.3f} | {med("lex_parse"):.3f} | {med("sema"):.3f} | {med("llvm_codegen"):.3f} | {med("packetize" if t=="riscv" else "spirv_emit"):.3f} | {med("write_output"):.3f} | {med("max_rss_kib")/1024:.1f} |')
    if result.get('io_samples'):
        lines+=['','## Controlled IR output experiment','', '| Destination | Median write ms | Median process ms |','|---|---:|---:|']
        for label in ['mounted_output','linux_temp','discard']:
            rows=[r for r in result['io_samples'] if r['destination']==label]
            lines.append(f'| {label} | {statistics.median(r["write_output"] for r in rows):.3f} | {statistics.median(r["process_ms"] for r in rows):.3f} |')
        lines+=['','Same 1,000-statement source and compiler executable; only output destination changes. File writes are flushed to the OS, not fsync-ed to durable storage. /dev/null still includes textual IR formatting cost.']
    if result.get('pipeline_samples'):
        lines+=['','## LLVM tool process timings','', '| Shader | Stage | Median ms | Min ms | Max ms |','|---|---|---:|---:|---:|']
        for r in result['pipeline_samples']:
            s=stats(r['samples_ms']); lines.append(f'| {r["case"]} | {r["stage"]} | {s["median"]:.3f} | {s["min"]:.3f} | {s["max"]:.3f} |')
    lines+=['','## Checks','']+['- '+c for c in checks]
    lines+=['','Raw samples: compiler_samples.csv, results.json. Exact commands: commands.json. LLVM pass detail: *.opt_O3.log and *.sincos.log. Optional CPU sampling: *.perf-report.txt.']
    for key,value in result.items():
        if key.endswith('_perf_unavailable'): lines+=['',key+': '+value]
    if errors: lines+=['','## Incomplete measurements','']+['```\n'+e+'\n```' for e in errors]
    (out/'report.md').write_text('\n'.join(lines)+'\n')
    print(f'Report: {out}/report.md',flush=True)
    return bool(errors)
if __name__=='__main__': sys.exit(main())
