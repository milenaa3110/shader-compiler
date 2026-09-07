# Whole-pipeline benchmarks

**No video** is the default for the run targets and benchmark hosts. The
complete rendering or compute workload is executed without ffmpeg being started
or images being saved. Texture and mesh inputs are still loaded normally.

```sh
cmake --build build --target rv-mandelbrot-scalar
cmake --build build --target rv-mandelbrot-packet
cmake --build build --target vk-mandelbrot
cmake --build build --target rv-mandelbrot-video
cmake --build build --target vk-mandelbrot-video
```

For graphics, `rv-<name>` is aliased to `rv-<name>-packet`. The same binary and
shader object are run by both modes; `SHADER_PACKET` is explicitly overridden by
`--packet-mode scalar|packet`, including an inherited value of `0`. VS and FS are
switched together. The actual stage modes, packet width, and mixed or all-scalar
fallback are reported in the startup output. Compute targets (`rv-life`,
`rv-blur`, `rv-volume-cs`) are dispatched through their existing compute row
dispatcher and are given no scalar/packet comparison.

Existing names are retained by the mesh targets, such as `rv-boss-scalar`,
`rv-boss-packet`, `vk-mesh-boss`, and `vk-mesh-boss-video`. For terrain,
`rv-terrain-scalar`, `rv-terrain-packet`, and `vk-terrain` are used. `--video` /
`--no-video` are also accepted by the volume and texture hosts; blur's explicit
output is written as a still PPM.

## Repeated comparisons

```sh
cmake --build build --target benchmark-fragment
cmake --build build --target benchmark-mesh
cmake --build build --target benchmark-vertex
cmake --build build --target benchmark-diverge
cmake --build build --target benchmark-compute
cmake --build build --target benchmark-compute-blur

# Select workloads and configuration directly; useful on the Banana Pi.
python3 test/script/benchmark_pipeline.py --build-dir build \
    --shaders mandelbrot julia --frames 60 --threads 8 --rv-only

# Reuse shader objects already built for this machine and packet width.
python3 test/script/benchmark_pipeline.py --build-dir build \
    --suite mesh --existing-objects --rv-only --threads 8
```

One complete warm-up invocation is given to each mode, followed by five measured
invocations. Scalar/packet order is alternated. Shader time, seeded inputs, and
stateful compute data are reset by fresh processes. Matching frame counts,
dimensions, geometry, and inputs are used on both graphics backends. The full
pipeline is measured for RV graphics: vertex processing, triangle setup, binning,
rasterization, fragment execution, and framebuffer writes. QEMU measurements are
labeled as emulated.

Medians and ranges are printed, and every raw host log and sample is retained in
`build/benchmark-results/<timestamp>-<suite>/results.json`. A result directory is
selected with `--output DIR`. Workload length is reduced by `--quick`, but five
repeats are retained. An additional video invocation is run after measurement
with `--render`; no video is explicitly requested with `--bench-only`. These are
also accepted through the shell entry points. Build failures are surfaced rather
than treated as successful runs.

Compute options include `--suite compute --grid 256 --gens 1000`, `--sweep`,
`--tiny`, `--suite compute-blur --runs 100`, and `--suite volume`. Every measured
Life workload is started from the same original seed, without it being advanced
in an internal warm-up.

## Timing definitions

| Metric | Interval |
|---|---|
| Vulkan device average | Timestamp queries around the render pass or compute dispatch batch, before readback copies |
| Vulkan host average | Command recording, submission, and fence wait; video encoding and image writes excluded |
| RISC-V average | Complete render pipeline or compute workload; video encoding and image writes excluded |
| Host wall total | Host setup, workload, optional output, and cleanup |

The selected queue's `timestampValidBits`, including counter wrap, and the
device's `timestampPeriod` are used for Vulkan timestamps. Query reset and
timestamp writes are placed outside render passes; 64-bit results and
availability are read after the completion fence. **unavailable** is reported for
unsupported or failed queries, never a CPU timing substitute. Device name and
type are recorded: llvmpipe and other CPU Vulkan implementations are labeled as
CPU renderers.

The fragment host's explicit `--bench` option is retained as a separate batched
submission throughput experiment. Per-frame submission is retained under normal
`--no-video`. The two modes must not be compared as if only video encoding
differed.

## Profiling and numerical checks

```sh
# Native RISC-V only; perf runs separately after timing, without video.
python3 test/script/benchmark_pipeline.py --build-dir build \
    --shaders mandelbrot --rv-only --threads 8 --perf
```

A locally unpacked perf executable is selected with `--perf-bin /path/to/perf`.
The raw counter report is saved per scalar/packet mode. No scalar/packet speedup
claim is made for an all-scalar fallback; mixed support is reported with the
actual stages.

Compiler fast-math settings are left unchanged. Scalar/packet numerical
differences under those settings must be reported as measured differences, not
automatically classified as packetizer failures. Captured-fragment replay and
numerical error metrics are retained in the separate
[packet diagnostic](PACKET_BENCHMARK.md). Tolerance failures under fast math
require a separate strict-FP comparison before their cause is attributed.

## Checks

```sh
ctest --test-dir build -R 'benchmark_modes|packet_' --output-on-failure
python3 test/script/test_benchmark_modes.py --build-dir build
```

All seven Vulkan hosts and their fixture shaders must be built for the second
command, along with a working Vulkan implementation and ffmpeg. Actual device
timestamp results, absence of ffmpeg/image output by default, the `--bench`
positional-argument regression, and explicit MP4 output are checked. The Vulkan
validation layer is enabled when installed. Option overrides, timestamp
reset/wrap/availability, metric selection, repeat ordering, and fallback
reporting are covered by unit tests.
