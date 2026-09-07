#!/usr/bin/env python3
"""Measure sincos-opt on a RISC-V Linux machine reached through SSH.

Example:
  python3 test/script/bench_sincos.py --host user@board --identity ~/.ssh/board_key
Uses local LLVM 18 and the project's built compiler/plugin, then the board's g++
and libm. Results, generated code and exact commands are saved locally.
"""
import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import math
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import statistics as st
import subprocess
import tarfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CASES = ['sincos_heavy', 'city', 'julia', 'matrix', 'ocean', 'scene3d', 'mandelbrot']
HEAVY = """uniform float uTime;
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment) fn void main() {
    float x = vUV.x * 6.0 + vUV.y + uTime;
    float total = 0.0;
    for (float i = 0.0; i < 24.0; i = i + 1.0) {
        float s = sin(x);
        float c = cos(x);
        total = total + s * 0.7 + c * 0.3;
        x = x + 0.13 + s * 0.01 + c * 0.02;
    }
    FragColor = vec4(total * 0.02 + 0.5, x * 0.02, vUV.y, 1.0);
}
"""
REMOTE_INFO = r"""import json,os,pathlib,shutil,subprocess
def command(args):
    p=subprocess.run(args,capture_output=True,text=True)
    return {'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
info={'home':str(pathlib.Path.home()),'machine':os.uname().machine,
      'uname':command(['uname','-a']),'cpu':command(['lscpu']),
      'libc':command(['ldd','--version']),'compiler':command(['g++','--version']),
      'uptime':command(['uptime']),'os_release':pathlib.Path('/etc/os-release').read_text(),
      'affinity':sorted(os.sched_getaffinity(0)),
      'tools':{x:shutil.which(x) for x in ['g++','perf','taskset','tar']}}
info['cpu_policies']={}
for p in sorted(pathlib.Path('/sys/devices/system/cpu/cpufreq').glob('policy*')):
    info['cpu_policies'][p.name]={q.name:q.read_text().strip() for q in p.iterdir() if q.name in
      ['scaling_governor','scaling_cur_freq','scaling_min_freq','scaling_max_freq','related_cpus']}
info['temperatures']={}
for p in pathlib.Path('/sys/class/thermal').glob('thermal_zone*'):
    info['temperatures'][p.name]={k:(p/k).read_text().strip() for k in ['type','temp'] if (p/k).exists()}
print(json.dumps(info))
"""

def quantile(values, p):
    values = sorted(values)
    x = (len(values) - 1) * p
    i = int(x)
    return values[i] + (values[min(i + 1, len(values) - 1)] - values[i]) * (x - i)

def interval(values):
    rng = random.Random(314159)
    resamples = [st.median(rng.choices(values, k=len(values))) for _ in range(3000)]
    return [quantile(resamples, .025), quantile(resamples, .975)]

def summarize(samples):
    grouped = {}
    for row in samples:
        grouped.setdefault((row['case'], row['mode']), []).append(row)
    result = []
    for (case, mode), rows in sorted(grouped.items()):
        pairs = {}
        for row in rows:
            pairs.setdefault((row['layout'], row['round']), {})[row['variant']] = row
        if any(set(pair) != {'off', 'on'} for pair in pairs.values()):
            raise ValueError('Incomplete A/B sample pair')
        ratios = [p['off']['wall_ms'] / p['on']['wall_ms'] for p in pairs.values()]
        off = [p['off']['wall_ms'] for p in pairs.values()]
        on = [p['on']['wall_ms'] for p in pairs.values()]
        layouts = {}
        for layout in sorted({key[0] for key in pairs}):
            selected = [p for key, p in pairs.items() if key[0] == layout]
            r = [p['off']['wall_ms'] / p['on']['wall_ms'] for p in selected]
            layouts[layout] = {'paired_speedup': st.median(r), 'ci95': interval(r)}
        result.append({
            'case': case, 'mode': mode, 'pairs': len(pairs),
            'off_median_ms': st.median(off), 'on_median_ms': st.median(on),
            'off_iqr_ms': [quantile(off, .25), quantile(off, .75)],
            'on_iqr_ms': [quantile(on, .25), quantile(on, .75)],
            'paired_speedup': st.median(ratios), 'ci95': interval(ratios),
            'time_reduction_percent': 100 * (1 - 1 / st.median(ratios)),
            'cpu_time_speedup': st.median([p['off']['cpu_ms'] / p['on']['cpu_ms'] for p in pairs.values()]),
            'layouts': layouts,
        })
    return result

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', required=True, help='SSH user@host or configured alias')
    ap.add_argument('--identity', type=Path, help='Private-key path (only passed to SSH)')
    ap.add_argument('--build-dir', type=Path, default=ROOT / 'build/profile-release')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--cases', nargs='+', choices=DEFAULT_CASES, default=DEFAULT_CASES)
    ap.add_argument('--rounds', type=int, default=31)
    ap.add_argument('--fragments', type=int, default=4096)
    ap.add_argument('--sample-ms', type=int, default=50)
    ap.add_argument('--time', type=float, default=1.7)
    ap.add_argument('--cpu', type=int, default=7)
    ap.add_argument('--layouts', nargs='+', choices=['off-first', 'on-first'], default=['off-first', 'on-first'])
    a = ap.parse_args()
    if a.host.startswith('-') or any(c.isspace() for c in a.host):
        ap.error('host must be one SSH destination')
    if min(a.rounds, a.fragments, a.sample_ms) < 1 or a.cpu < 0 or not math.isfinite(a.time):
        ap.error('invalid benchmark dimensions')
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%d-%H%M%S')
    out = (a.output or ROOT / ('build/sincos-board-' + stamp)).resolve()
    if out.exists() and any(out.iterdir()):
        ap.error('output directory is not empty; choose a new directory')
    out.mkdir(parents=True, exist_ok=True)
    build = a.build_dir.resolve()
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=10']
    if a.identity:
        ssh += ['-o', 'IdentitiesOnly=yes', '-i', str(a.identity.expanduser().resolve())]
    ssh += [a.host]
    commands, errors, metadata, records, samples = [], [], {}, [], []
    result = {
        'created_utc': stamp, 'settings': {k: str(v) if isinstance(v, Path) else v for k, v in vars(a).items()},
        'method': 'One CPU; both variants in one executable with isolated globals and the same timed output buffers; same O3 IR; alternating balanced randomized A/B order; two object link orders; board system libm; no LTO.',
        'scope': 'Shader invocation throughput including packet packing/scattering; excludes renderer, image/video I/O, compilation and SSH latency. Packet width comes from the existing compiler build.',
        'validation_scope': 'Each scalar/packet mode compares pass off/on at uTime 0, 1.7 and 5 with finite random UV inputs; not an exhaustive floating-point proof.',
        'cases': metadata, 'records': records, 'errors': errors,
    }
    env = os.environ.copy()
    for key in ['SHADER_PROFILE', 'SHADER_EMIT_PACKET', 'IRGEN_SPIRV_DUMP_IR', 'SHADER_PACKET']:
        env.pop(key, None)

    def run(argv, data=None, log=None, timeout=180):
        argv = [str(x) for x in argv]
        commands.append({'argv': argv, 'stdin_sha256': hashlib.sha256(data).hexdigest() if data is not None else None})
        start = time.perf_counter()
        p = subprocess.run(argv, input=data, capture_output=True, cwd=ROOT, env=env, timeout=timeout)
        elapsed = (time.perf_counter() - start) * 1000
        if log:
            (out / log).write_bytes(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(shlex.join(argv) + ' failed:\n' + p.stderr.decode(errors='replace')[-4000:]
                               + '\n' + p.stdout.decode(errors='replace')[-2000:])
        return p.stdout, p.stderr, elapsed

    def remote(argv, data=None, **kwargs):
        return run(ssh + [shlex.join([str(x) for x in argv])], data=data, **kwargs)

    def inventory():
        stdout, _, _ = remote(['python3', '-'], data=REMOTE_INFO.encode())
        return json.loads(stdout)

    try:
        for tool in ['opt-18', 'llc-18', 'riscv64-linux-gnu-nm', 'riscv64-linux-gnu-objcopy', 'llvm-size-18', 'ssh']:
            if not shutil.which(tool):
                raise RuntimeError('Missing local tool: ' + tool)
        result['llvm_version'] = run(['opt-18', '--version'])[0].decode()
        result['git_head'] = run(['git', 'rev-parse', 'HEAD'])[0].decode().strip()
        result['git_status'] = run(['git', 'status', '--short'])[0].decode()
        result['board_before'] = inventory()
        board = result['board_before']
        if board['machine'] != 'riscv64' or a.cpu not in board['affinity']:
            raise RuntimeError('Board architecture or requested CPU is unavailable')
        if not all(board['tools'][x] for x in ['g++', 'taskset', 'tar']):
            raise RuntimeError('Board needs g++, taskset and tar')
        remote_dir = str(Path(board['home']) / '.cache/shader-compiler' / ('sincos-' + stamp + '-' + uuid.uuid4().hex[:6]))
        result['remote_directory'] = remote_dir
        compiler, plugin = build / 'riscv/irgen_riscv', build / 'llvm/sincos_opt.so'
        if not compiler.is_file() or not plugin.is_file():
            raise RuntimeError('Build irgen_riscv and sincos_opt first')
        (out / 'bench_sincos.cpp').write_bytes((ROOT / 'test/bench_sincos.cpp').read_bytes())
        upload = ['bench_sincos.cpp']
        width = None
        for case in a.cases:
            source = out / (case + '.src') if case == 'sincos_heavy' else ROOT / ('test/shaders/animations/' + case + '_fs.src')
            if case == 'sincos_heavy':
                source.write_text(HEAVY)
            raw, opt, transformed = [out / (case + suffix) for suffix in ['.raw.ll', '.off.ll', '.on.ll']]
            run([compiler, raw], data=source.read_bytes(), log=case + '.frontend.log')
            ir = raw.read_text()
            w = int(re.search(r'@__shader_packet_width\s*=.*constant i32 (\d+)', ir)[1])
            if width is None:
                width = w
            if w != width or a.fragments % w:
                raise RuntimeError('Packet width mismatch or fragment count is not divisible by width')
            run(['opt-18', '-O3', '--enable-unsafe-fp-math', '--fp-contract=fast',
                 '-verify-each', '-S', raw, '-o', opt], log=case + '.O3.log')
            _, _, process_ms = run(['opt-18', '-load-pass-plugin=' + str(plugin), '-passes=sincos-opt',
                                   '-verify-each', '-time-passes', '-S', opt, '-o', transformed],
                                  log=case + '.sincos-pass.log')
            before, after = opt.read_bytes(), transformed.read_bytes()
            metadata[case] = {
                'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                'packet_width': width, 'has_packet': bool(re.search(rb'^define .*@fs_packet\(', before, re.M)),
                'sincos_ir_calls_off': len(re.findall(rb'\bcall\b[^\n]*@sincosf\(', before)),
                'sincos_ir_calls_on': len(re.findall(rb'\bcall\b[^\n]*@sincosf\(', after)),
                'local_sincos_opt_process_ms': process_ms,
                'variants': {},
            }
            for variant, path in [('off', opt), ('on', transformed)]:
                obj, asm = out / (case + '.' + variant + '.original.o'), out / (case + '.' + variant + '.s')
                base = ['llc-18', '-O3', '--fp-contract=fast', '-relocation-model=pic',
                        '-mtriple=riscv64-unknown-linux-gnu', '-mattr=+m,+a,+f,+d,+v', path]
                run(base + ['-filetype=obj', '-o', obj], log=case + '.' + variant + '.llc.log')
                run(base + ['-filetype=asm', '-o', asm])
                assembly = asm.read_text()
                function_calls, function_hashes = {}, {}
                for m in re.finditer(r'^\s*\.type\s+([^,]+),@function\s*\n(.*?)^\s*\.size\s+\1,', assembly, re.M | re.S):
                    name, body = m.group(1).strip(), m.group(2)
                    function_hashes[name] = hashlib.sha256(body.encode()).hexdigest()
                    calls = re.findall(r'^\s*(?:call|tail)\s+((?:sin|cos|sincos)f)(?:@plt)?\s*$', body, re.M)
                    if calls:
                        function_calls[name] = {name: calls.count(name) for name in sorted(set(calls))}
                size = run(['llvm-size-18', '--format=sysv', obj])[0].decode()
                text_bytes = sum(int(n) for n in re.findall(r'^\.text\S*\s+(\d+)', size, re.M))
                metadata[case]['variants'][variant] = {
                    'object_sha256': hashlib.sha256(obj.read_bytes()).hexdigest(),
                    'text_bytes': text_bytes, 'math_call_sites_by_function': function_calls,
                    'function_assembly_sha256': function_hashes,
                }
                symbols = run(['riscv64-linux-gnu-nm', '-g', '--defined-only', '--format=posix', obj])[0].decode()
                rename = out / (case + '.' + variant + '.symbols')
                rename.write_text(''.join(line.split()[0] + ' ' + variant + '_' + line.split()[0] + '\n'
                                          for line in symbols.splitlines() if line.strip()))
                prefixed = out / (case + '.' + variant + '.o')
                run(['riscv64-linux-gnu-objcopy', '--redefine-syms=' + str(rename), obj, prefixed])
                upload.append(prefixed.name)
            metadata[case]['object_identical_before_prefixing'] = (
                metadata[case]['variants']['off']['object_sha256'] == metadata[case]['variants']['on']['object_sha256'])
            hashes = [metadata[case]['variants'][v]['function_assembly_sha256'] for v in ['off', 'on']]
            metadata[case]['unchanged_function_assembly'] = [name for name in hashes[0] if hashes[0][name] == hashes[1].get(name)]
            print('Prepared ' + case + ': ' + str(metadata[case]['sincos_ir_calls_on']) + ' sincos IR call sites', flush=True)
        result['packet_width'] = width
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:gz') as tar:
            for filename in upload:
                tar.add(out / filename, arcname=filename)
        remote(['mkdir', '-p', remote_dir])
        remote(['tar', '-xzf', '-', '-C', remote_dir], data=archive.getvalue())
        remote(['g++', '-std=c++20', '-O3', '-march=rv64gcv', '-mabi=lp64d',
                '-DSHADER_PACKET_WIDTH=' + str(width), '-c', remote_dir + '/bench_sincos.cpp',
                '-o', remote_dir + '/harness.o'], log='board-harness-build.log')
        for case in a.cases:
            for layout in a.layouts:
                order = ['off', 'on'] if layout == 'off-first' else ['on', 'off']
                exe = remote_dir + '/' + case + '.' + layout
                remote(['g++', '-O3', '-march=rv64gcv', '-mabi=lp64d', remote_dir + '/harness.o',
                        *[remote_dir + '/' + case + '.' + v + '.o' for v in order], '-lm', '-o', exe],
                       log=case + '.' + layout + '.link.log')
        print('Board builds complete; starting one-core A/B measurements.', flush=True)
        schedule = [(case, mode, layout) for case in a.cases for mode in ['scalar', 'packet']
                    if mode == 'scalar' or metadata[case]['has_packet'] for layout in a.layouts]
        random.Random(123).shuffle(schedule)
        for job, (case, mode, layout) in enumerate(schedule, 1):
            tag = case + '.' + mode + '.' + layout
            board_sample = inventory()
            stdout, _, _ = remote(['taskset', '-c', str(a.cpu), remote_dir + '/' + case + '.' + layout,
                                  mode, str(a.rounds), str(a.fragments), str(a.sample_ms), str(a.time), str(1000 + job)],
                                 log=tag + '.jsonl', timeout=max(180, a.rounds * 10))
            parsed = [json.loads(line) for line in stdout.decode().splitlines() if line.strip()]
            for record in parsed:
                record.update(case=case, mode=mode, layout=layout)
                records.append(record)
                if record['kind'] == 'sample':
                    samples.append(record)
            result.setdefault('board_samples', []).append({'job': tag, 'before': board_sample})
            print('Measured ' + tag + ' (' + str(job) + '/' + str(len(schedule)) + ')', flush=True)
        result['board_after'] = inventory()
        result['summary'] = summarize(samples)
    except Exception as ex:
        errors.append(str(ex))
        print('ERROR: ' + str(ex), flush=True)
    finally:
        result['commands'] = commands
        (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        if samples:
            with (out / 'samples.csv').open('w', newline='') as f:
                w = csv.DictWriter(f, fieldnames=list(samples[0]))
                w.writeheader()
                w.writerows(samples)
        lines = ['# sincos-opt on RISC-V hardware', '', result['scope'], '', result['method'], '',
                 'Run: ' + stamp + ' UTC', 'Board: ' + a.host, 'Pinned CPU: ' + str(a.cpu),
                 'Arguments: ' + shlex.join(['python3', 'test/script/bench_sincos.py', *os.sys.argv[1:]]), '',
                 'The off branch is the same optimized IR immediately before sincos-opt. The on branch adds only sincos-opt.',
                 'Both use llc-18 with the same options and link against the board system math library.',
                 'Output validation, warmup, calibration, checksums and printing are outside the timed samples.',
                 'Each sample repeats a complete fragment batch; reported times are milliseconds per batch.',
                 'Speedup is the median paired off/on wall-time ratio. CI95 is a bootstrap interval for that median.',
                 'The interval describes these samples and does not cover every source of systematic error.',
                 'Link order is swapped to expose sensitivity to code placement. CPU-time ratios help identify descheduling.',
                 'The one-shot local opt process timing includes startup, parsing and writing; it is not isolated pass cost.',
                 'No performance counters are collected by this script.', '']
        if result.get('summary'):
            lines += ['| Shader | Mode | Off ms | On ms | Speedup | CI95 | Time reduction | Off-first / on-first |',
                      '|---|---|---:|---:|---:|---|---:|---|']
            for row in result['summary']:
                layouts = ' / '.join(f'{x["paired_speedup"]:.4f}x' for x in row['layouts'].values())
                lines.append(f'| {row["case"]} | {row["mode"]} | {row["off_median_ms"]:.4f} | {row["on_median_ms"]:.4f} | '
                             f'{row["paired_speedup"]:.4f}x | [{row["ci95"][0]:.4f}, {row["ci95"][1]:.4f}] | '
                             f'{row["time_reduction_percent"]:.3f}% | {layouts} |')
        lines += ['', '## Code generation', '',
                  '| Shader | Fused calls in on IR | Off text bytes | On text bytes | Original objects identical |',
                  '|---|---:|---:|---:|---|']
        for case, m in metadata.items():
            if len(m['variants']) != 2:
                continue
            lines.append(f'| {case} | {m["sincos_ir_calls_on"]} | {m["variants"]["off"]["text_bytes"]} | '
                         f'{m["variants"]["on"]["text_bytes"]} | {m["object_identical_before_prefixing"]} |')
        lines += ['', '## Validation', '', result['validation_scope']]
        validations = [r for r in records if r['kind'] == 'validation']
        if validations:
            lines += [f'Compared {sum(r["components"] for r in validations):,} components across all runs.',
                      f'Max absolute difference: {max(r["max_abs_error"] for r in validations):.9g}.',
                      f'Mismatches: {sum(r["mismatches"] for r in validations)}; nonfinite components: {sum(r["nonfinite"] for r in validations)}.']
        if errors:
            lines += ['', '## Errors', *errors]
        lines += ['', 'Full commands, library/OS/compiler versions, per-function call counts, temperature/frequency snapshots,',
                  'validation records and raw samples are in results.json, samples.csv and the individual logs.']
        (out / 'report.md').write_text('\n'.join(lines) + '\n')
        print('Report: ' + str(out / 'report.md'), flush=True)
    return 1 if errors else 0

if __name__ == '__main__':
    raise SystemExit(main())
