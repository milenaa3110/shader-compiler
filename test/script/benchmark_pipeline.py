#!/usr/bin/env python3
"""Whole-workload Vulkan/scalar/packet measurements, with raw repeat logs."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
ANIMATIONS = "mandelbrot julia voronoi waves tunnel ripple galaxy fire reaction cellular earth scene3d city ocean matrix".split()
NUMBER = r"([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)"


def metric(output, label):
    match = re.search(re.escape(label) + r"\s*" + NUMBER + r"\s+ms", output)
    return float(match[1]) if match else None


def parse_sample(output, vulkan):
    device = re.search(r"Vulkan device: (.*\(type=.*\))", output)
    mode = re.search(r"\[pipeline\] mode=(\S+) VS=(\S+) FS=(\S+) packet_width=(\d+) fallback=(\S+)", output)
    rv_ms = metric(output, "RISC-V avg:")
    if rv_ms is None:
        rv_ms = metric(output, "CPU avg:")
    if rv_ms is None:
        rv_ms = metric(output, "[life-cpu] avg:")
    return {
        "ms": metric(output, "Vulkan device avg:") if vulkan else rv_ms,
        "host_ms": metric(output, "Vulkan host avg:") if vulkan else None,
        "wall_ms": metric(output, "Host wall total:"),
        "device": device[1] if device else None,
        "stages": dict(zip(("mode", "vs", "fs", "width", "fallback"), mode.groups())) if mode else None,
    }


def summary(samples, field="ms"):
    values = [s[field] for s in samples]
    if not values or any(v is None for v in values):
        return {"median_ms": None, "min_ms": None, "max_ms": None}
    return {"median_ms": statistics.median(values), "min_ms": min(values), "max_ms": max(values)}


def execute(command, log, env):
    start = time.monotonic()
    result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"Command exited {result.returncode}; see {log}\n{result.stdout[-1500:]}")
    return result.stdout, (time.monotonic() - start) * 1000


def measure(commands, directory, env, repeats=5):
    directory.mkdir(parents=True, exist_ok=True)
    samples = {mode: [] for mode in commands}
    active = dict(commands)
    errors = {}
    for mode, command in commands.items():
        try:
            execute(command + ["--no-video"], directory / f"{mode}-warmup.log", env)
        except RuntimeError as error:
            if mode != "vulkan" or len(commands) == 1:
                raise
            errors[mode] = str(error)
            del active[mode]
            print(f"Vulkan unavailable: {error}", flush=True)
    # Fresh processes reset state/time/input before each complete workload.
    for repeat in range(repeats):
        order = list(active)
        if repeat % 2:
            order.reverse()
        for mode in order:
            output, elapsed = execute(commands[mode] + ["--no-video"],
                                      directory / f"{mode}-{repeat + 1}.log", env)
            sample = parse_sample(output, mode == "vulkan")
            sample.update(process_wall_ms=elapsed, repeat=repeat + 1)
            if sample["ms"] is None and mode != "vulkan":
                raise RuntimeError(f"Missing RV timing in {directory}/{mode}-{repeat + 1}.log")
            if mode == "vulkan" and "Vulkan device avg:" not in output:
                raise RuntimeError("Vulkan host did not report timestamp availability; rebuild it")
            samples[mode].append(sample)
    result = {"commands": commands, "samples": samples, "errors": errors,
              "summary": {mode: summary(data) for mode, data in samples.items()}}
    result["host_summary"] = {mode: summary(data, "host_ms") for mode, data in samples.items() if mode == "vulkan"}
    result["wall_summary"] = {mode: summary(data, "wall_ms") for mode, data in samples.items()}
    scalar = result["summary"].get("scalar", {}).get("median_ms")
    packet = result["summary"].get("packet", {}).get("median_ms")
    stages = samples.get("packet", [{}])[0].get("stages")
    result["scalar_over_packet"] = scalar / packet if scalar and packet and stages and stages["fallback"] != "all-scalar" else None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("fragment", "mesh", "vertex", "diverge", "compute", "compute-blur", "volume"), default="fragment")
    parser.add_argument("--build-dir", type=Path, default=Path(os.environ.get("BUILD_DIR", "build")))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--shaders", nargs="+")
    parser.add_argument("--frames", type=int)
    parser.add_argument("--size", type=int)
    parser.add_argument("--threads", type=int, default=int(os.environ.get("NTHREADS", os.cpu_count() or 1)))
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--quick", action="store_true")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--rv-only", action="store_true")
    modes.add_argument("--vk-only", action="store_true")
    parser.add_argument("--existing-objects", action="store_true", help="Reuse compiled shaders; relink current RV hosts")
    video = parser.add_mutually_exclusive_group()
    video.add_argument("--render", "--video", "--animate", action="store_true", help="Record separately after measurements")
    video.add_argument("--bench-only", "--no-video", action="store_true")
    parser.add_argument("--perf", action="store_true", help="Run perf stat separately on the built RV executable")
    parser.add_argument("--perf-bin", default="perf")
    parser.add_argument("--mesh", default=str(ROOT / "test/assets/boss/Rumba Dancing.obj"))
    parser.add_argument("--grid", type=int, default=256)
    parser.add_argument("--gens", type=int)
    parser.add_argument("--runs", type=int, default=int(os.environ.get("NRUNS", "100")))
    parser.add_argument("--sweep", action="store_true")
    parser.add_argument("--tiny", action="store_true")
    parser.add_argument("--volume", action="store_true", help="Compute volume ray-march")
    args = parser.parse_args()
    if args.volume:
        args.suite = "volume"
    frames = args.frames if args.frames is not None else (10 if args.quick else 60)
    compute = args.suite in ("compute", "compute-blur", "volume")
    if args.suite == "compute":
        frames = args.gens if args.gens is not None else (100 if args.quick else 1000)
    elif args.suite == "compute-blur":
        frames = 10 if args.quick else args.runs
    size = args.size if args.size is not None else {"mesh": 768, "compute": 128 if args.quick else args.grid, "volume": 256}.get(args.suite, 512)
    unit = "gen" if args.suite == "compute" else "run" if args.suite == "compute-blur" else "frame"
    if min(frames, size, args.threads, args.repeats) <= 0:
        parser.error("frames, size, threads and repeats must be positive")
    if args.suite == "mesh" and size != 768:
        parser.error("the Vulkan mesh host uses 768x768; omit --size")
    names = args.shaders or {"fragment": ANIMATIONS, "mesh": ["mesh"],
                            "vertex": ["terrain"], "diverge": ["mandelbrot", "diverge"],
                            "compute": ["life"], "compute-blur": ["blur"], "volume": ["volume_cs"]}[args.suite]
    build = args.build_dir.resolve()
    out = (args.output or build / "benchmark-results" / (time.strftime("%Y%m%d-%H%M%S") + "-" + args.suite)).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = str(args.threads)
    env["OMP_DYNAMIC"] = "FALSE"
    width_file = build / "packet_width.env"
    if not width_file.exists():
        parser.error(f"Missing {width_file}; configure the build first")
    width = int(re.search(r"SHADER_PACKET_WIDTH=(\d+)", width_file.read_text())[1])
    native = platform.machine() == "riscv64"
    if args.perf and not native:
        parser.error("--perf requires native RISC-V execution")
    compiler = "g++" if native else "riscv64-linux-gnu-g++"
    simulator = [] if native else [shutil.which("qemu-riscv64-static") or shutil.which("qemu-riscv64") or "qemu-riscv64", "-L", "/usr/riscv64-linux-gnu"]
    cache = build / "CMakeCache.txt"
    if "VK_ICD_FILENAMES" not in env and cache.exists():
        icd = re.search(r"^DZN_ICD:FILEPATH=(.+)$", cache.read_text(), re.M)
        if icd:
            env["VK_ICD_FILENAMES"] = icd[1]
            env["LD_LIBRARY_PATH"] = "/usr/lib/wsl/lib:" + env.get("LD_LIBRARY_PATH", "")
    print(f"Whole workload: {size}x{size}, {frames} {unit}s, {args.threads} threads, {args.repeats} repeats", flush=True)
    print("RV execution: " + ("native RISC-V" if native else "QEMU emulation"), flush=True)
    results = {"config": vars(args) | {"frames": frames, "size": size, "packet_width": width,
                                      "rv_execution": "native" if native else "QEMU"}, "workloads": {}}
    workloads = [(name, size) for name in names]
    if args.suite == "compute" and args.sweep:
        workloads = [("life", grid) for grid in (16, 32, 64, 128, 256, 512)]
    elif args.suite == "compute" and args.tiny and size != 32:
        workloads.append(("life", 32))
    for name, size in workloads:
        mesh = args.suite == "mesh"
        vert = "mesh" if mesh else "terrain" if name == "terrain" else "quad"
        host = {"life": "spirv_vulkan_life_host", "blur": "spirv_vulkan_compute_host",
                "volume_cs": "spirv_vulkan_volume_cs_host"}.get(name, "spirv_vulkan_mesh_host" if mesh else "spirv_vulkan_host")
        shader = "volume" if name == "volume_cs" else name
        object_name = name + ("_cs_rv.o" if name in ("life", "blur") else "_rv.o")
        targets = ([] if args.rv_only else [host, f"{vert}.vert.spv", f"{name}.frag.spv"])
        if compute and not args.rv_only:
            targets = [host, f"{shader}.comp.spv"]
        if not args.vk_only:
            targets += ["volume_cs.rv" if name == "volume_cs" else object_name]
        if not args.existing_objects:
            subprocess.run(["cmake", "--build", str(build), "--parallel", str(min(args.threads, 8)), "--target", *targets], cwd=ROOT, check=True)
        commands = {}
        key = f"{name}-{size}" if name == "life" else name
        directory = out / key
        directory.mkdir(exist_ok=True)
        if not args.rv_only:
            commands["vulkan"] = list(map(str, [build / "spirv" / host, build / "spirv" / f"{vert}.vert.spv",
                                               build / "spirv" / f"{name}.frag.spv", name, frames]))
            commands["vulkan"] += [args.mesh] if mesh else [str(size), str(size), "6144" if name == "terrain" else "6"]
            if compute:
                arguments = [frames, size] if name == "life" else [name, frames, size, size] if name == "volume_cs" else [name, frames]
                commands["vulkan"] = list(map(str, [build / "spirv" / host, build / "spirv" / f"{shader}.comp.spv", *arguments]))
        if not args.vk_only:
            binary = directory / f"{name}.rv"
            source = {"life": "compute", "blur": "compute_blur", "volume_cs": "volume_cs"}.get(name, "mesh" if mesh else "fragment")
            command = [compiler, "-std=c++20", "-O3", "-static", "-fopenmp", "-march=rv64gcv", "-mabi=lp64d",
                       f'-DANIM_NAME="{name}"', f"-DNFRAMES={frames}", f"-DWIDTH={size}", f"-DHEIGHT={size}",
                       f"-DGRID={size}", f"-DNGENERATIONS={frames}", f"-DNRUNS={frames}",
                       f"-DSHADER_PACKET_WIDTH={width}", f"-DVERT_COUNT={6144 if name == 'terrain' else 6}",
                       str(ROOT / f"test/rv_host/rv_host_{source}.cpp")]
            if not compute:
                command += [str(ROOT / "src/runtime/pipeline_runtime.cpp")]
            command += [str(build / "riscv" / object_name), "-o", str(binary)]
            execute(command, directory / "build.log", env)
            for mode in (("rv",) if compute else ("scalar", "packet")):
                commands[mode] = simulator + [str(binary)] + ([name, str(frames), args.mesh] if mesh else []) + (["--packet-mode", mode] if not compute else [])
        print(f"Measuring {key}...", flush=True)
        result = measure(commands, directory, env, args.repeats)
        result.update(size=size, units=frames, unit=unit)
        results["workloads"][key] = result
        (out / "results.json").write_text(json.dumps(results, indent=2, default=str) + "\n")
        for mode, data in result["summary"].items():
            value = data["median_ms"]
            print(f"  {mode}: " + (f"{value:.6f} ms/{unit} [{data['min_ms']:.6f}, {data['max_ms']:.6f}]" if value is not None else "unavailable"))
            first = result["samples"][mode][0] if result["samples"][mode] else {}
            if first.get("device"):
                print("    " + first["device"])
            if first.get("stages"):
                print("    " + str(first["stages"]))
        ratio = result["scalar_over_packet"]
        host = result["host_summary"].get("vulkan", {}).get("median_ms")
        if host is not None:
            print(f"  Vulkan host: {host:.6f} ms/{unit}")
        for mode, data in result["wall_summary"].items():
            if data["median_ms"] is not None:
                print(f"  {mode} host wall total: {data['median_ms']:.3f} ms")
        if not compute:
            print("  scalar/packet: " + (f"{ratio:.3f}x" if ratio else "N/A"), flush=True)
        if args.perf:
            if not native:
                raise RuntimeError("perf requires native RISC-V execution")
            for mode in ("scalar", "packet", "rv"):
                if mode in commands:
                    execute([args.perf_bin, "stat", "-e", "cycles,instructions,branches,branch-misses", "--"] + commands[mode] + ["--no-video"], directory / f"perf-{mode}.log", env)
        if args.render:
            for mode, command in commands.items():
                if mode != "scalar" and mode not in result["errors"]:
                    execute(command + ["--video"], directory / f"{mode}-video.log", env)
    print(f"Raw samples and summary: {out / 'results.json'}")


if __name__ == "__main__":
    main()
