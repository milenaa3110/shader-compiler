#!/usr/bin/env python3
import argparse, pathlib, subprocess, re, json

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build/bpi-f3-w8'
OUT = BUILD / 'riscv'
BOARD = 'milenajovanovic@192.168.0.114'
KEY = '/root/.ssh/banana_pi_f3_bench'
REMOTE = '/tmp/sincos-bench'

def run(cmd, **kw):
    return subprocess.run([str(x) for x in cmd], check=True, text=True,
                          capture_output=True, **kw)

def main():
    ap = argparse.ArgumentParser(description='Build and run scalar/packet A/B fragment benchmarks on the BPI-F3')
    ap.add_argument('--names', nargs='*', help='animation names (default: every *_fs.src)')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--fragments', type=int, default=16384)
    args = ap.parse_args()
    compiler = BUILD / 'riscv/irgen_riscv'
    names = args.names or sorted(p.stem[:-3] for p in (ROOT/'test/shaders/animations').glob('*_fs.src'))
    rows=[]
    for name in names:
        src = ROOT/'test/shaders/animations'/f'{name}_fs.src'
        raw,opt,obj = OUT/f'bpi_{name}.raw.ll',OUT/f'bpi_{name}.ll',OUT/f'bpi_{name}.o'
        try:
            p=subprocess.run([str(compiler),str(raw)],stdin=src.open('rb'),capture_output=True,text=True,check=False)
        except OSError as e:
            rows.append({'name':name,'status':'tool-unavailable','error':str(e)}); continue
        if p.returncode:
            rows.append({'name':name,'status':'compile-failed','error':p.stderr[-300:]}); continue
        ir=raw.read_text(errors='replace')
        if not re.search(r'^define .*@fs_packet\(',ir,re.M):
            rows.append({'name':name,'status':'not-packetizable'}); continue
        try:
            run(['opt-18','-O3','--enable-unsafe-fp-math','--fp-contract=fast','-S',raw,'-o',opt])
            run(['llc-18','-O3','--fp-contract=fast','-filetype=obj','-relocation-model=pic',
                 '-mtriple=riscv64-unknown-linux-gnu','-mattr=+m,+a,+f,+d,+v',opt,'-o',obj])
            run(['scp','-q','-i',KEY,obj,f'{BOARD}:{REMOTE}/'])
            exe=f'{REMOTE}/bpi_{name}'
            run(['ssh','-i',KEY,BOARD,'g++','-O3','-march=rv64gcv','-mabi=lp64d','-fopenmp',
                 '-DSHADER_PACKET_WIDTH=8',f'{REMOTE}/profile_packet.cpp',f'{REMOTE}/bpi_{name}.o',
                 '-lm','-o',exe])
            q=subprocess.run(['ssh','-i',KEY,BOARD,'taskset','-c','0',exe,'1',str(args.runs),str(args.reps),str(args.fragments)],capture_output=True,text=True,check=False)
            rows.append({'name':name,'status':'tested' if q.returncode == 0 else 'runtime-failed','returncode':q.returncode,'output':q.stdout.strip(),'error':q.stderr.strip()})
        except (subprocess.CalledProcessError, OSError) as e:
            rows.append({'name':name,'status':'build-or-link-failed','error':str(e)})
    print(json.dumps(rows,indent=2))

if __name__=='__main__': main()
