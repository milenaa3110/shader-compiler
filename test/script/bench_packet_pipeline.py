#!/usr/bin/env python3
"""Native scalar/packet benchmark. Run on RISC-V, using framework shader objects.

Example: python3 test/script/bench_packet_pipeline.py --build-dir build --perf
Use --existing-objects to benchmark already-built objects without rebuilding shaders.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(cmd, log, timeout=1200, env=None):
    with log.open('w') as f:
        p = subprocess.Popen(list(map(str, cmd)), stdout=f, stderr=subprocess.STDOUT,
                             start_new_session=True, env=env)
        try:
            return p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            # perf/compiler processes may own children; stop the whole invocation.
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            raise

def report(rows, out):
    lines=['# Packet benchmark results', '',
           'Ratios greater than 1 mean packet is faster. Failed validation makes timings diagnostic only.',
           'See manifest.json for resolution, frame count, build hashes, and thread settings.', '',
           '| Shader | Threads | Validation | Phase | Scalar ms/frame | Packet ms/frame | Scalar / packet |',
           '|---|---:|---|---|---:|---:|---:|']
    for row in rows:
        if row.get('error'):
            lines.append(f"| {row['name']} | - | {row['error']} | - | - | - | - |")
        for tr in row['threads']:
            status=tr['validation'] if tr.get('fs_packet') else 'no packet entry'
            for phase,values in tr['timings'].items():
                def val(mode):
                    return f"{values[mode]['median_ms']:.3f}" if mode in values else '-'
                ratio=f"{values['scalar_over_packet']:.3f}" if 'scalar_over_packet' in values else '-'
                lines.append(f"| {row['name']} | {tr['threads']} | {status} | {phase} | {val('scalar')} | {val('packet')} | {ratio} |")
    (out/'README.md').write_text('\n'.join(lines)+'\n')

def record_profile(perf, workload, stem, out, timeout, env):
    result={}
    for event in ['cycles:u','cpu-clock:u']:
        suffix='' if event=='cycles:u' else '.cpu-clock'
        data=out/(stem+suffix+'.perf.data')
        log=out/(stem+suffix+'.perf-record.log')
        report_path=out/(stem+suffix+'.perf-report.txt')
        rc=run([perf,'record','-F','99','-e',event,'--call-graph','dwarf,8192','-o',data,'--']+workload,log,timeout,env)
        rr=run([perf,'report','--stdio','--no-children','-i',data],report_path,timeout,env) if rc==0 else None
        # perf 6.1 can return success while printing "data has no samples".
        content=report_path.read_text() if report_path.exists() else ''
        sampled=rr==0 and 'Samples:' in content and 'has no samples' not in content
        result={'record_event':event,'record_returncode':rc,'report_returncode':rr,'samples_available':sampled}
        if sampled: break
    return result

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-dir', type=Path, default=ROOT/'build')
    ap.add_argument('--out-dir', type=Path)
    ap.add_argument('--names', nargs='+')
    ap.add_argument('--threads', nargs='+', type=int, default=[1, 8])
    ap.add_argument('--frames', type=int, default=60)
    ap.add_argument('--rounds', type=int, default=5)
    ap.add_argument('--width', type=int, default=512)
    ap.add_argument('--height', type=int, default=512)
    ap.add_argument('--phase', choices=['all','check','replay','pipeline'], default='all')
    ap.add_argument('--existing-objects', action='store_true')
    ap.add_argument('--perf', action='store_true', help='perf stat and call graph for each pipeline mode')
    ap.add_argument('--perf-bin', default='perf')
    ap.add_argument('--timeout', type=int, default=1800, help='seconds per shader/thread invocation')
    a = ap.parse_args()
    if min(a.threads + [a.frames,a.rounds,a.width,a.height,a.timeout]) < 1:
        ap.error('numeric arguments must be positive')
    if platform.machine() != 'riscv64':
        ap.error('run on native riscv64 hardware; QEMU timings are not supported')
    build = a.build_dir.resolve()
    out = (a.out_dir or build/'packet-benchmark'/time.strftime('%Y%m%d-%H%M%S')).resolve()
    out.mkdir(parents=True, exist_ok=False)
    framework = ROOT/'test/script/run_benchmark_fragment.sh'
    names = a.names or re.search(r'^ANIMATIONS=\(([^)]+)\)', framework.read_text(), re.M).group(1).split() + ['mesh']
    if any(not re.fullmatch(r'[a-zA-Z0-9_]+', n) for n in names):
        ap.error('invalid shader name')
    width_file = build/'packet_width.env'
    if not width_file.exists():
        ap.error('build is missing packet_width.env; configure the framework first')
    pw = int(width_file.read_text().strip().split('=')[1])
    if pw not in (4,8,16): ap.error('unsupported packet width')
    env = os.environ.copy()
    env.pop('SHADER_PACKET', None)
    env.update(OMP_DYNAMIC='FALSE', OMP_PROC_BIND='close', OMP_PLACES='cores')
    manifest = {'arguments':vars(a).copy(), 'host':platform.uname()._asdict(),
                'packet_width':pw, 'objects':{}, 'sources':{},
                'environment':{k:env.get(k) for k in ['OMP_DYNAMIC','OMP_PROC_BIND','OMP_PLACES']}}
    for path in [ROOT/'test/bench_packet_pipeline.cpp',ROOT/'src/runtime/pipeline_runtime.cpp',
                 ROOT/'src/runtime/pipeline_runtime.h',ROOT/'src/runtime/pipeline_abi.h']:
        manifest['sources'][str(path.relative_to(ROOT))] = sha(path)
    for name in ['CMakeCache.txt','packet_width.env']:
        if (build/name).exists(): shutil.copy2(build/name,out/name)
    rows=[]
    flags=['g++','-std=c++20','-O3','-g','-fno-omit-frame-pointer','-fopenmp',
           '-march=rv64gcv','-mabi=lp64d',f'-DSHADER_PACKET_WIDTH={pw}','-DSHADER_PACKET_BENCH']
    runtime_obj=out/'pipeline_runtime.o'
    if run(flags+['-c',ROOT/'src/runtime/pipeline_runtime.cpp','-o',runtime_obj],out/'runtime.link.log'):
        raise RuntimeError('runtime compilation failed; see runtime.link.log')
    host_objects={}
    for name in names:
        print(f'{name}: preparing',flush=True)
        row={'name':name,'threads':[]}
        rows.append(row)
        try:
            if not a.existing_objects:
                rc=run(['cmake','--build',build,'--target',name+'_rv.o','--parallel','2'],out/f'{name}.build.log')
                if rc: raise RuntimeError(f'shader build failed ({rc}); see build log')
            obj=build/'riscv'/f'{name}_rv.o'
            if not obj.exists(): raise RuntimeError('framework object missing')
            manifest['objects'][name]={'path':str(obj),'sha256':sha(obj)}
            # Scalar fs_invoke does not expose a discard mask; don't invent a reference.
            source=ROOT/'test/shaders/animations'/f'{name}_fs.src'
            if source.exists() and re.search(r'\bdiscard\b', '\n'.join(l.split('#')[0] for l in source.read_text().splitlines())):
                raise RuntimeError('discard shader requires a scalar liveness ABI; unsupported by this harness')
            exe=out/name
            adapter='mesh' if name=='mesh' else 'fullscreen'
            if adapter not in host_objects:
                host=out/f'{adapter}.host.o'
                cmd=flags+(['-DPACKET_BENCH_MESH'] if adapter=='mesh' else [])
                rc=run(cmd+['-c',ROOT/'test/bench_packet_pipeline.cpp','-o',host],out/f'{adapter}.host.log')
                if rc: raise RuntimeError('host compilation failed; see host log')
                host_objects[adapter]=host
            cmd=flags+[host_objects[adapter],runtime_obj,obj,'-lm','-o',exe]
            rc=run(cmd,out/f'{name}.link.log')
            if rc: raise RuntimeError(f'harness link failed ({rc}); see link log')
            manifest['objects'][name]['executable_sha256']=sha(exe)
            for threads in a.threads:
                print(f'{name}: {threads} threads, {a.phase}',flush=True)
                base=[exe,'--threads',threads,'--frames',a.frames,'--rounds',a.rounds,'--width',a.width,'--height',a.height]
                log=out/f'{name}.{threads}t.jsonl'
                rc=run(base+['--phase',a.phase],log,a.timeout,env)
                records=[json.loads(l) for l in log.read_text().splitlines() if l.startswith('{')]
                complete=next((r for r in records if r['kind']=='complete'),{})
                config=next((r for r in records if r['kind']=='configuration'),{})
                tr={'threads':threads,'returncode':rc,'validation':complete.get('validation','incomplete'),
                    'fs_packet':config.get('fs_packet'), 'timings':{},'checks':[r for r in records if r['kind']=='check']}
                row['threads'].append(tr)
                for phase in ['replay','pipeline']:
                    values={}
                    for mode in ['scalar','packet']:
                        samples=[r for r in records if r['kind']=='sample' and r['phase']==phase and r['mode']==mode]
                        # Replay reports per-frame samples. Aggregate a complete sequence per round.
                        by_round={}
                        for r in samples: by_round.setdefault(r['round'],[]).append(r['ms'])
                        v=[statistics.mean(ms) for ms in by_round.values()]
                        if v: values[mode]={'median_ms':statistics.median(v),'min_ms':min(v),'max_ms':max(v),'round_ms':v}
                    if 'scalar' in values and 'packet' in values:
                        values['scalar_over_packet']=values['scalar']['median_ms']/values['packet']['median_ms']
                    if values: tr['timings'][phase]=values
                print(f"{name}: {threads}t validation={tr['validation']} timings={tr['timings']}",flush=True)
                if a.perf:
                    perf=shutil.which(a.perf_bin)
                    if not perf:
                        tr['perf']={'status':'unavailable','reason':'perf executable not found'}
                    elif not config.get('fs_packet'):
                        tr['perf']={'status':'skipped','reason':'no packet entry'}
                    else:
                        # Whole-process counters, including fixed warmup; separate from render timers.
                        tr['perf']={}
                        for mode in ['scalar','packet']:
                            workload=base+['--phase','pipeline','--mode',mode]
                            stem=f'{name}.{threads}t.{mode}'
                            stats={}
                            # One event per execution avoids PMU multiplexing. Do not derive
                            # exact IPC by dividing counters collected in different processes.
                            for event in ['task-clock','cycles:u','instructions:u','branches:u','branch-misses:u','cache-misses:u']:
                                event_name=event.replace(':','-')
                                counters=out/(stem+f'.{event_name}.perf-stat.csv')
                                rcstat=run([perf,'stat','-x,','-o',counters,'-e',event,'--']+workload,out/(stem+f'.{event_name}.perf-stat.log'),a.timeout,env)
                                counter_text=counters.read_text() if counters.exists() else ''
                                stats[event]={'returncode':rcstat,'counted':bool(counter_text.strip()) and rcstat==0 and '<not ' not in counter_text}
                            tr['perf'][mode]={'stat':stats,**record_profile(perf,workload,stem,out,a.timeout,env)}
            row['status']='tested'
        except (RuntimeError,OSError,ValueError,subprocess.TimeoutExpired) as e:
            row['status']='error'; row['error']=str(e)
            print(f'{name}: {e}',flush=True)
        finally:
            (out/'summary.json').write_text(json.dumps(rows,indent=2))
            (out/'manifest.json').write_text(json.dumps(manifest,indent=2,default=str))
            report(rows,out)
    print(f'Results: {out}',flush=True)
    return int(any(r.get('status')=='error' or any(t['returncode'] for t in r['threads']) for r in rows))

if __name__=='__main__':
    raise SystemExit(main())
