# Fragment packetization benchmark

For the standard no-video comparison in which both VS and FS are switched, the
[whole-pipeline benchmarks](BENCHMARKING.md) should be used. In this diagnostic
VS execution is held fixed while fragment packetization and numerical differences
are examined.

To be run on native RISC-V hardware in a configured framework checkout:

```sh
python3 test/script/bench_packet_pipeline.py --build-dir build --perf
```

The animation list from `run_benchmark_fragment.sh` is used by default, plus the
framework's `icosphere:3` mesh scene, 512x512, 60 frames at 30 fps, 5 paired
rounds, and separate 1-thread and 8-thread experiments. A subset is selected with
`--names waves mesh`. The build's existing `_rv.o` files are used with
`--existing-objects`; otherwise their normal CMake targets are built first by the
runner. Shader and runtime packet widths must match. Shader objects are identical
for both modes, including their optimization flags and sincos pass. The harness is
compiled once per scene adapter, without LTO, with debug information for
profiling.

Correctness and median/min/max milliseconds per frame over complete animation
sequences are reported in `summary.json`. The speed ratio is scalar time / packet
time; values greater than one mean packet is faster. Raw frame replay samples are
kept in the per-shader/thread JSONL logs. Source/object/executable SHA-256 hashes,
machine information, arguments, and OpenMP affinity settings are recorded in
`manifest.json`. The build configuration is copied alongside the results. A short
verification run can be requested with `--frames 3 --rounds 3`; it must be
labeled as a short run, not the default clip.

## What is measured

* **Correctness:** every requested animation frame, plus `uTime=5`. The real SoA
  packet calls and the valid lane count, including tail padding, are captured by
  the runtime. The same inputs are replayed by the scalar and packet functions
  with unchanged uniforms/textures. All float output components are compared with
  `abs(a-b) <= 1e-4 + 1e-4*max(abs(a),abs(b))`; nonfinite outputs are failed.
  Mismatch counts, maximum absolute/relative error, and live-mask mismatches are
  included in the reports. RGB bytes are compared between separate scalar/packet
  renders, with one quantization level allowed. Floating-point differences can
  fail even when images look alike; performance from failed cases is diagnostic
  only.
* **Fragment replay:** the captured packet groups are stored as AoS fragments.
  Scalar calls or SoA packing, packet calls, and output scattering are timed
  during replay. The same inputs, grouping, and output allocation are used by the
  two modes. They are warmed per captured frame and measured in seeded random
  paired order. Capture, allocations, validation, checksums, and rasterization are
  excluded. Static batch scheduling is used for replay; the runtime's dynamic tile
  scheduler and the cache state created by ongoing rasterization are not
  reproduced.
* **Pipeline:** the production `render_pipeline()`, with vertex processing,
  rasterization, depth tests, packet assembly, shading, and RGB output included.
  The complete clip is warmed for each measured mode before repeated paired
  sequences. Checksums and printing are placed outside the timer. No video encoder
  is launched. Vertex execution is held scalar in both modes so that fragment
  packetization is isolated. Fixed team sizes and `close`/`cores` affinity are used
  for OpenMP.

Benchmark hooks exist only under `SHADER_PACKET_BENCH` in the runtime. No capture
branch or benchmark dispatch override is present in normal renderer builds. Actual
linked `fs_packet` availability is reported and the packet-width ABI is validated
by the benchmark. A missing entry is explicitly skipped, with a scalar-only
pipeline baseline; it is never reported as packet performance.

## Scope

The framework's generated geometry, interpolated varyings, and fragment
coordinates are used for fullscreen animations. The same generated `icosphere:3`,
warm beige material, and white fallback texture as the default mesh renderer are
used for mesh. Adapters that capture per-draw uniform and texture state are needed
for other OBJ/material scenes; they are not covered by this default mesh case.
Shaders with explicit `discard` are rejected by the runner: the current scalar ABI
has no liveness return value, so a reliable discard reference cannot be supplied.
Live lanes are checked against the all-live expectation for the supported,
non-discard suite. Storage-image side effects are excluded from this replay
experiment.

## perf

Separate scalar and packet **pipeline-only** executions are added by `--perf`
for:

* `perf stat`: task clock, user cycles/instructions, branches, branch misses,
  and cache misses. Each event is run separately so that PMU multiplexing is
  avoided; availability and running-time status are retained in the raw CSV.
  Ratios across separate executions are estimates, not exact same-run IPC.
* `perf record`: 99 Hz user-cycle sampling with DWARF call graphs, followed by a
  saved `perf report --stdio`. If cycle sampling fails or yields no report, user
  CPU-clock sampling is retried and the selected event is recorded. Debug symbols
  are retained in the executables; generated shader functions have symbols but may
  not have source-level debug information.

A kernel tools binary outside PATH is selected with `--perf-bin /path/to/perf`.
Missing tools, denied counters, unsupported events, and recording/report failures
are saved explicitly. These are whole-process profiles, including startup, warmup,
and checksums. They are not counters restricted to the renderer timer, and their
instrumented elapsed times must not replace the unprofiled A/B timings. Cycles,
instructions, IPC, and symbol hotspots must be compared only alongside correctness
and counter availability. No global performance/security settings are changed.

A saved executable can be profiled manually:

```sh
perf stat -e cycles:u,instructions:u -- /path/to/results/waves \
  --phase pipeline --mode scalar --threads 8 --frames 60 --rounds 5
perf record -F 99 -e cycles:u --call-graph dwarf,8192 -- /path/to/results/waves \
  --phase pipeline --mode packet --threads 8 --frames 60 --rounds 5
```

## Harness regression check

```sh
python3 test/script/test_packet_benchmark.py
```

The real rasterizer is driven by this native-host test at an odd 17x13 resolution
with two threads. Complete pixel coverage, tail packets, successful replay,
deliberate output and mask failures, and scalar fallback with no packet symbol are
checked.
