#!/usr/bin/env python3
"""Exercise capture/replay, tails, failure reporting, and fallback on a native host."""
import json
import argparse
from pathlib import Path
import subprocess
import tempfile
import bench_packet_pipeline as runner

ROOT=Path(__file__).resolve().parents[2]
MOCK=r'''
#include <cstdlib>
#include <limits>
extern "C" {
int vs_total_floats=6,vs_varying_floats=2,vs_input_floats=0,vs_input_doubles=0;
int fs_output_floats=4,fs_output_doubles=0;
extern const int __shader_packet_width=4;
float uTime=0;
void vs_invoke(int id,int,float*,double*,float* o) {
    const float x[6]={-1,1,1,-1,1,-1},y[6]={-1,-1,1,-1,1,1};
    o[0]=x[id];o[1]=y[id];o[2]=0;o[3]=1;o[4]=(x[id]+1)/2;o[5]=(y[id]+1)/2;
}
void fs_invoke(float*,float* v,float* o,double*) {o[0]=v[0];o[1]=v[1];o[2]=uTime/10;o[3]=1;}
#ifndef NO_PACKET
void fs_packet(const float* v,const float*,float* o,int* live) {
    for(int l=0;l<4;++l) {o[l]=v[l];o[4+l]=v[4+l];o[8+l]=uTime/10;o[12+l]=1;live[l]=-1;}
    if(std::getenv("BAD_OUTPUT")) o[0]+=0.5f;
    if(std::getenv("BAD_MASK")) live[0]=0;
    if(std::getenv("BAD_NAN")) o[0]=std::numeric_limits<float>::quiet_NaN();
}
#endif
}
'''

def main():
    import os
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sanitize',action='store_true')
    args=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='packet-benchmark-test-') as td:
        d=Path(td); (d/'shader.cpp').write_text(MOCK)
        # perf on the board returns zero even when its report has no samples.
        calls=[]
        def fake_perf(cmd, log, timeout, env):
            calls.append(cmd)
            log.write_text('Error: data has no samples!\n' if len(calls)==2 else '# Samples: 50 of event cpu-clock:u\n')
            return 0
        original_run=runner.run
        runner.run=fake_perf
        try:
            result=runner.record_profile('perf',['shader'],'profile',d,10,{})
            assert result['record_event']=='cpu-clock:u' and result['samples_available'] and len(calls)==4
        finally:
            runner.run=original_run
        common=['g++','-std=c++20','-O2','-fopenmp','-DSHADER_PACKET_BENCH','-DSHADER_PACKET_WIDTH=4',
                str(ROOT/'test/bench_packet_pipeline.cpp'),str(ROOT/'src/runtime/pipeline_runtime.cpp'),str(d/'shader.cpp')]
        if args.sanitize: common += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        for fallback in [False,True]:
            exe=d/('fallback' if fallback else 'packet')
            subprocess.run(common+(['-DNO_PACKET'] if fallback else [])+['-o',str(exe)],check=True)
            args=[str(exe),'--width','17','--height','13','--frames','2','--rounds','2','--threads','2']
            for fault in ([None] if fallback else [None,'BAD_OUTPUT','BAD_MASK','BAD_NAN']):
                env=os.environ.copy()
                env.pop('BAD_OUTPUT',None); env.pop('BAD_MASK',None); env.pop('BAD_NAN',None)
                if fault: env[fault]='1'
                p=subprocess.run(args,text=True,capture_output=True,env=env)
                assert p.returncode==int(bool(fault)),p.stderr+p.stdout
                rows=[json.loads(l) for l in p.stdout.splitlines() if l.startswith('{')]
                assert rows[-1]['validation']==('not_run' if fallback else 'failed' if fault else 'passed')
                if not fallback:
                    checks=[r for r in rows if r['kind']=='check']
                    assert len(checks)==3 and all(r['fragments']==17*13 for r in checks),checks
                    assert len([r for r in rows if r['kind']=='sample' and r['phase']=='pipeline'])==4
                else:
                    assert any(r['kind']=='skip' for r in rows)
        print('PASS: capture, tails, 2-thread replay, output/mask/NaN failures, scalar and perf fallbacks')

if __name__=='__main__': main()
