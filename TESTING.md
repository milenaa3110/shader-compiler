# Testing Guide

Command reference for building, testing, and benchmarking the compiler on Linux
or WSL. **No video** is the default for benchmark hosts: the workload is executed
without ffmpeg being started or screenshots being saved. `-video` targets or the
runner's `--render` option must be used when output is wanted.

Vulkan, RISC-V scalar, and RISC-V packet execution of the **complete pipeline**
are compared by the standard graphics benchmarks, with both VS and FS switched.
Details are given in [benchmark timing and methodology](test/BENCHMARKING.md).
Captured-fragment replay, numerical error metrics, and profiling are provided by
the separate [fragment packet diagnostic](test/PACKET_BENCHMARK.md), with VS
execution kept fixed.

## Prerequisites

```bash
# Core build (always required): CMake 3.20+, C++20 compiler, LLVM 18 toolchain
sudo apt install cmake build-essential llvm-18 clang-18

# Libraries and tools required by the build and scripts
sudo apt install libfmt-dev libvulkan-dev mesa-vulkan-drivers vulkan-tools \
    spirv-headers spirv-tools python3

# Optional: MP4 output and Vulkan API validation
sudo apt install ffmpeg vulkan-validationlayers

# Cross-compiling RISC-V from an x86 host (QEMU) — not needed on RISC-V hardware
sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu qemu-user-static
```

Notes:
- Vulkan + `spirv-headers` are checked at configure time even for non-GPU targets.
  LavaPipe is supplied by `mesa-vulkan-drivers` so that Vulkan targets are run
  without a GPU; those measurements are labeled as CPU Vulkan, not hardware GPU time.
- LLVM 17 is auto-detected as a fallback if `llvm-18` is absent.
- On native RISC-V hardware (e.g. Banana Pi F3), the core packages and required
  libraries must be installed, but the cross-compiler/QEMU packages can be skipped.
  The native toolchain is used by the scripts; `.rv` binaries are run directly and
  RVV is executed on-core.
- ffmpeg is unnecessary for normal benchmark runs. A working `perf` is required
  for native profiling with `--perf`; its executable is selected with
  `--perf-bin /path/to/perf`.

## Build

```bash
cmake -S . -B build                # one-time configure
cmake --build build -j$(nproc)     # build all tools + sincos_opt.so
cmake --build build --target clean # remove generated artifacts
```

Artifacts: `build/spirv/` (SPIR-V + Vulkan hosts), `build/riscv/` (`.ll`/`.o`/`.rv`),
`build/llvm/` (pass plugin). Explicit video/image output is placed in `result/`.
Repeated benchmark logs and JSON summaries are written to
`build/benchmark-results/<timestamp>-<suite>/` by default.
From inside `build/`, plain `make <target>` is also accepted.

For a separate width-8 build targeting Banana Pi F3:

```bash
cmake -S . -B build/bpi-f3-w8 -DCMAKE_BUILD_TYPE=Release -DSHADER_PACKET_WIDTH=8
cmake --build build/bpi-f3-w8 -j$(nproc)
```

`--build-dir build/bpi-f3-w8` must be passed to benchmark scripts when that build
is used. The same packet width must be used by the shader objects and the runtime.

---

## Tests

### 1. Compiler unit tests
```bash
cmake --build build --target check          # compile every compiler_tests/*.src + llvm-as
ctest --test-dir build                      # run every correctness gate below
```

The correctness gates (each is a CTest case; one can be run directly with its script):

| Gate | Script | What it checks |
|------|--------|----------------|
| `lexer_test` | `build/lexer_test` | Unit test of the buffer-backed lexer |
| `sema_test` | `build/sema_test` | Type-system rules: result types + rejected constructs (no codegen) |
| `ir_checks` | `test/script/run_ir_checks.sh` | FileCheck over emitted IR shape (needs FileCheck-18) |
| `reject_tests` | `test/script/run_reject_tests.sh` | Shaders that must fail with an exact diagnostic; a crash is a failure |
| `matrix_numeric` | `test/script/run_matrix_numeric.sh` | Emitted IR is linked to a hand-computed C++ driver and column-major results are compared (needs clang-18) |
| `sampler_numeric` | `test/script/run_sampler_numeric.sh` | Real per-face/slice/layer/texel data is bound and sampled values are checked on the RISC-V path (needs clang-18) |
| `packet_test` | `test/script/run_packet_test.sh` | `@fs_packet` is emitted and grepped for the width-agnostic vectorized IR shape (select/if-else/discard/loop) |
| `packet_runtime` | `test/script/run_packet_runtime.sh` | A packet build is run under QEMU: VLEN banner smoke + build-width guard (marker≠runtime → loud abort) (needs cross-cc + QEMU) |
| `packet_benchmark_harness` | `test/script/test_packet_benchmark.py` | Captured-fragment replay, packet tails, numerical/failure reporting, and fallback |
| `benchmark_modes` | `test/script/test_benchmark_modes.py` | No-video options, scalar/packet overrides, timestamp reset/wrap/availability, metric parsing, repeat order, and fallback reporting |
| `texlayer_rv` | `test/rv_host/rv_host_texlayer.cpp` | A cube/3D/array/2D + image2D shader is rendered under QEMU, bound via `bind_texture_layered`/`bind_texture`/`bind_image`; the pixel + image round-trip is checked (needs cross-cc + QEMU) |
| `compare_backends` | `test/script/compare_backends.sh` | Descriptive SPIR-V-vs-RISC-V static metrics table (bytes/opcodes/insns); no pass/fail (needs python3) |
| `spirv_validate` | `test/script/run_spirv_val.sh` | `spirv-val` over every emitted `.spv` (needs spirv-val) |

Matrix support is covered end-to-end across these: the matrix type rules are
stamped by `sema_test`, arithmetic / component-wise ops / builtins (`transpose`,
`inverse`, `determinant`, `matrixCompMult`, `outerProduct`) / assignment / `mat++`
are verified by `matrix_numeric`, the diagnostics are pinned by `reject_tests`, and
`matdemo.vert.spv` is run by `spirv_validate` — the one shader by which the real
`OpTypeMatrix` path is exercised (ColMajor/MatrixStride uniforms, column indexing
incl. a *direct* index on a uniform matrix `uMVP[i]`, `mat == mat` in a bool
context, and a matrix varying).

Sampler/image support spans all types on both backends: `sampler2D/3D/Cube/2DArray`
+ `texture`/`textureLod`, and `image2D`/`imageBuffer` + `imageLoad`/`imageStore`.
Dispatch is done per dimension by the SPIR-V backend (real `OpTypeImage`
Cube/3D/2D-arrayed, `OpImageSampleExplicitLod`, `OpImageRead`/`OpImageWrite`),
covered by `cubemap.frag.spv` and `imagestore.comp.spv` under `spirv_validate`;
the build is now failed loudly on an unlowered sampler/image intrinsic instead of
an invalid module being written. Each intrinsic is implemented against real bound
data by the RISC-V runtime (`src/runtime/tex_inline.cpp`) — cube-face selection,
3D trilinear, array-layer select, and storage load/store, indexed by the sampler's
binding slot. Real per-face/slice/layer/texel data is bound by the
`sampler_numeric` CTest and the sampled values are checked (incl. an
`imageStore`→`imageLoad` round-trip and that `textureLod` is clamped to the base
level). The render-pipeline loop is closed by the `texlayer_rv` CTest: a
cube/3D/array/2D texture and a read/write image2D are bound via
`bind_texture_layered`/`bind_texture`/`bind_image` and `texlayer_fs` is rendered
under QEMU, with the output pixel and the image round-trip checked end-to-end.

> **Known limitation — reduced opt is used for layered render shaders.**
> cube/3D/array/image sampling is miscompiled by `opt -O3` on the RISC-V backend:
> when the `always_inline` sampler bodies (by which `float* out` is written, read
> back as `<4 x float>` under `vscale_range`) are inlined into the shader, a
> NULL-deref / wrong pixel is produced (LLVM-18 backend bug, reproduced at
> `llc -O0`, scalar and vector). The runtime is correct — `sampler_numeric` and the
> x86 path are passed, and bit-exact rendering is obtained from a **non-inlining**
> opt pipeline. So the `texlayer_rv` shader object is built with a curated pipeline
> (mem2reg/sroa/gvn/dse/… — no inliner) + `llc -O3` (see the CMake comment). The
> aggressive `-O3` is kept for 2D-only render shaders; the bug is not hit by them.
> Remaining follow-up: mip pyramids (`textureLod` is clamped to the base level) and
> root-causing the `-O3` inliner miscompile upstream.

### 2. Vulkan animations (single shader)
```bash
cmake --build build --target vk-mandelbrot       # 300 frames, no video
cmake --build build --target vk-mandelbrot-video # 300 frames → result/mandelbrot.mp4
# also: vk-julia vk-voronoi vk-waves vk-tunnel vk-ripple vk-galaxy vk-fire
#       vk-reaction vk-cellular vk-earth vk-scene3d vk-diverge vk-city vk-ocean vk-matrix
cmake --build build --target all-vk              # build every procedural animation shader
```
Device selection: a real GPU is picked, with fallback to LavaPipe (stderr warning)
when none is present. The device name and type are included in the output. A
device is forced with `VK_DEVICE_INDEX=<n>`.

Terrain, texture, and volume:
```bash
cmake --build build --target vk-terrain
cmake --build build --target vk-texture
cmake --build build --target vk-volume
cmake --build build --target vk-volume-cs
# Each also has a -video target.

# Manual terrain run: the final positional argument is the vertex count.
build/spirv/spirv_vulkan_host build/spirv/terrain.vert.spv \
    build/spirv/terrain.frag.spv terrain 60 512 512 6144 --no-video
```

### 3. RISC-V CPU animations (same shaders, different backend)
```bash
cmake --build build --target rv-mandelbrot-scalar # whole pipeline, scalar VS + FS
cmake --build build --target rv-mandelbrot-packet # whole pipeline, available packet VS + FS
cmake --build build --target rv-mandelbrot        # alias for -packet, no video
cmake --build build --target rv-mandelbrot-video  # packet mode with MP4 output
cmake --build build --target all-rv               # build every RISC-V animation binary
```
The same binary and shader object are used by the scalar and packet targets. An
inherited `SHADER_PACKET` value is overridden by the explicit `--packet-mode`
option. Actual VS/FS modes, packet width, and partial or all-scalar fallback are
identified in the startup output. The same target suffixes are accepted for
terrain and graphics volume.

Built binaries are run directly on native RISC-V:
```bash
OMP_NUM_THREADS=8 build/riscv/mandelbrot.rv --packet-mode scalar --no-video
OMP_NUM_THREADS=8 build/riscv/mandelbrot.rv --packet-mode packet --no-video

cmake --build build --target texture_test.rv
OMP_NUM_THREADS=8 build/riscv/texture_test.rv --packet-mode packet --video
```
On x86/WSL, `qemu-riscv64-static -L /usr/riscv64-linux-gnu` must be inserted
before the `.rv` binary. Emulation is described by QEMU timings, not native
RISC-V performance.

### 4. Whole-pipeline fragment benchmark
```bash
cmake --build build --target benchmark-fragment        # 15 shaders, 60 frames each
cmake --build build --target benchmark-fragment-quick  # 10 frames; still five repeats

bash test/script/run_benchmark_fragment.sh --rv-only --threads 8
bash test/script/run_benchmark_fragment.sh --shaders mandelbrot julia --frames 60
bash test/script/run_benchmark_fragment.sh --quick --render # extra video run after timing
```

`test/script/benchmark_pipeline.py` is used by all six benchmark entry points.
Shared options include `--build-dir`, `--threads`, `--repeats`, `--rv-only`,
`--vk-only`, `--existing-objects`, and `--output`. The default no-video behavior is
explicitly selected by `--bench-only` / `--no-video`; separate recording is done by
`--render` / `--video` after the timed invocations.

One complete warm-up invocation and five measured invocations are received by each
mode by default, with scalar/packet order alternated. Matching frame counts,
dimensions, and inputs are used for graphics workloads. Raw logs and JSON samples
are retained in the reports, median/min/max are shown, and the speedup claim is
omitted if packet mode falls back entirely to scalar. Their existing row
dispatcher is used for compute workloads and one RV column is reported.

| Reported metric | What it measures |
|-----------------|------------------|
| `Vulkan device avg` | Timestamp queries around the render pass or compute batch, before readback copies |
| `Vulkan host avg` | Command recording, submission, and fence wait; encoding and image writes excluded |
| `RISC-V avg` | Complete graphics pipeline or compute workload; encoding and image writes excluded |
| `Host wall total` | Setup, workload, optional output, and cleanup |

`unavailable` is reported for unsupported Vulkan timestamp queries, without CPU
timing being substituted. CPU Vulkan devices are identified explicitly. The
fragment host's `--bench` is retained as a separate batched-submission throughput
mode; it is not an alias for normal `--no-video` execution.

### 5. Terrain vertex-shader benchmark
```bash
cmake --build build --target benchmark-vertex
bash test/script/run_benchmark_vertex.sh
```
The complete 6144-vertex terrain pipeline is used at 512×512, with 60 frames per
invocation by default, and Vulkan and both RV modes are compared.

### 5b. Indexed-mesh demo (VBO + IBO)
```bash
cmake --build build --target vk-mesh         # GPU,  1280 tris   (rv-mesh    = CPU)
cmake --build build --target vk-mesh-hi      # GPU, 20480 tris   (rv-mesh_hi = CPU)

# OBJ assets:
#   vk-mesh-bunny / rv-bunny   Stanford bunny, 4968 tris
#   vk-mesh-jeep  / rv-jeep    textured vehicle, 4728 tris
#   vk-mesh-teddy / rv-teddy   high-poly teddy, 1.5M tris (CPU slow — minutes)
#   vk-mesh-boss  / rv-boss    textured Mixamo character, 10220 tris

cmake --build build --target rv-boss-scalar
cmake --build build --target rv-boss-packet
cmake --build build --target vk-mesh-boss-video
cmake --build build --target rv-boss-video
```
The same source (`mesh_vs.src` + `mesh_fs.src`) is used for both backends;
rendered at 768×768, 300 frames are run by the standalone targets without video
unless `-video` is selected. A negative-height viewport is used by Vulkan so that
the image orientation is aligned.

### 5c. Indexed-mesh benchmark
```bash
cmake --build build --target benchmark-mesh
bash test/script/run_benchmark_mesh.sh
bash test/script/run_benchmark_mesh.sh --rv-only --threads 8 --frames 60
```
The textured boss model is used at 768×768. Another OBJ is selected with
`--mesh PATH`, or `--mesh icosphere:3` for procedural geometry.

### 6. Game of Life benchmark (multi-pass)
```bash
cmake --build build --target benchmark-compute

bash test/script/run_benchmark_compute.sh --tiny            # default grid plus 32×32
bash test/script/run_benchmark_compute.sh --sweep           # grid sizes 16→512
bash test/script/run_benchmark_compute.sh --animate         # extra video run after timing
bash test/script/run_benchmark_compute.sh --grid 128 --gens 500
cmake --build build --target vk-life                       # standalone, no video
cmake --build build --target rv-life-video                 # standalone with video
```
Each measured invocation is started from the original seeded state. Snapshot
output is excluded from workload timing. The compute volume ray-march is selected
instead of Life by `--volume`.

### 7. Branch-divergence benchmark
```bash
cmake --build build --target benchmark-diverge
bash test/script/run_benchmark_diverge.sh --quick
```
Mandelbrot and the divergent shader are compared using the same whole-pipeline
methodology, dimensions, and frame counts as the fragment benchmark. Both RV
stages are switched together. `scalar/packet` is scalar time divided by packet
time; values above one mean packet mode is faster.

### 8. Compute shader benchmark (Gaussian blur)
```bash
cmake --build build --target benchmark-compute-blur
bash test/script/run_benchmark_compute_blur.sh
bash test/script/run_benchmark_compute_blur.sh --runs 20 --rv-only --threads 8
```
Default workload: 512×512, 100 dispatches per invocation. No image is written
unless output is requested; blur's explicit output is a still PPM.

### 9. CPU thread scaling + RVV vector width
```bash
cmake --build build --target cpu-scaling         # full 5-section analysis (~10 min)
bash test/script/run_cpu_scaling.sh [--quick|--rvv-only]
```

### 10. Backend representation comparison (static)
```bash
cmake --build build --target compare-backends    # builds artifacts, prints table
bash test/script/compare_backends.sh [--csv]     # if artifacts already built
```

### 11. SPMD packetizer (VLEN-adaptive SIMD)
```bash
bash test/script/run_packet_test.sh     # emit-only: width-agnostic vectorized IR shape (select/if-else/discard/loop)
bash test/script/run_packet_runtime.sh  # under QEMU: VLEN banner + build-width guard (mismatch → loud abort)
python3 test/script/bench_packet_pipeline.py --build-dir build  # separate native packet diagnostic
```
The packet width is a single source of truth (`SHADER_PACKET_WIDTH`, default 4,
`src/common/packet_width.h`) baked into both `irgen_riscv` (`kW`) and the runtime
(`PACKET_W`). A `__shader_packet_width` marker is stamped into the shader `.rv` by
`irgen_riscv`; a mismatch is aborted on loudly by the runtime (the SoA stride would
otherwise silently corrupt every pixel). `vlenb` is probed by
`-DSHADER_PACKET_WIDTH=auto` on a native RISC-V build (`VLEN/32` f32 lanes); 4 is
the default for cross/QEMU builds. Modes are selected with `rv-<name>-scalar` /
`rv-<name>-packet`, or `--packet-mode scalar|packet` is passed directly to a
graphics host. `SHADER_PACKET=1` is still supported when no explicit mode is
supplied. The environment variable's presence is checked by the runtime, so scalar
execution is **not** selected by `SHADER_PACKET=0` or `SHADER_PACKET=`.

Native RISC-V measurements must be used for packet speedup to be assessed.
Emulation overhead is included in QEMU timing, and it is not established by
generated vector instructions alone that an entire pipeline is faster. Actual
stage fallback is reported by the runner alongside the measured ratio.

Compiler fast-math settings are unchanged by the comparison. Numerical
differences under those settings require a controlled strict-FP comparison
before they are attributed to the packetizer.

### Native full-pipeline profiling

```bash
python3 test/script/benchmark_pipeline.py --build-dir build/bpi-f3-w8 \
    --shaders mandelbrot julia --frames 60 --threads 8 --rv-only \
    --existing-objects --perf --output build/bpi-f3-w8/profile-results
```

Separate `perf stat` invocations are run by `--perf` on the built no-video
executables after timing samples have been collected. The tool is overridden by
`--perf-bin /path/to/perf`. `results.json`, per-repeat logs, and separate
scalar/packet counter logs are contained in the output directory.
`benchmark-packet-pipeline` or `test/script/bench_packet_pipeline.py` must be used
for the separate fragment replay diagnostic; sampling profiles are also supported
by its `--perf` option.

### Benchmark and Vulkan timing checks

```bash
ctest --test-dir build -R 'benchmark_modes|packet_' --output-on-failure

# Build the fixtures for actual Vulkan host checks.
cmake --build build --target \
    spirv_vulkan_host spirv_vulkan_mesh_host spirv_vulkan_texture_host \
    spirv_vulkan_volume_host spirv_vulkan_volume_cs_host \
    spirv_vulkan_life_host spirv_vulkan_compute_host \
    quad.vert.spv mandelbrot.frag.spv mesh.vert.spv mesh.frag.spv \
    texture_test.frag.spv volume.frag.spv volume.comp.spv life.comp.spv blur.comp.spv
python3 test/script/test_benchmark_modes.py --build-dir build
```

A working Vulkan implementation and ffmpeg are required by the integration check.
All seven hosts are checked for positive timestamp results and absence of
video/image output by default, explicit MP4 output is verified, and `--bench`
argument parsing is exercised. The Vulkan validation layer is enabled when
installed. Counter wrap, unavailable queries, repeat ordering, and scalar fallback
are also covered by unit checks.

### RVV instruction generation check
```bash
cmake --build build --target mandelbrot_rv.o
riscv64-linux-gnu-objdump -d build/riscv/mandelbrot_rv.o | grep -E 'vl[ew]|vf(add|mul|sub|div)|vset'
```

---

## Quick reference

```bash
cmake --build build --target check                     # compiler correctness (unit tests)
cmake --build build --target vk-mandelbrot             # Vulkan, no video
cmake --build build --target vk-mandelbrot-video       # Vulkan with MP4 output
cmake --build build --target rv-mandelbrot-scalar      # full scalar pipeline, no video
cmake --build build --target rv-mandelbrot-packet      # full packet pipeline, no video
cmake --build build --target rv-mandelbrot-video       # packet pipeline with MP4 output
cmake --build build --target all-vk                    # build every Vulkan shader
cmake --build build --target all-rv                    # build every RISC-V shader
cmake --build build --target benchmark-fragment-quick  # Vulkan/scalar/packet, 15 shaders
cmake --build build --target benchmark-vertex          # complete terrain pipeline
cmake --build build --target benchmark-mesh            # complete textured mesh pipeline
cmake --build build --target vk-mesh                   # indexed icosphere (1280 tris), GPU
cmake --build build --target rv-mesh                   # indexed icosphere (1280 tris), CPU
cmake --build build --target vk-mesh-boss              # textured Mixamo character, GPU
cmake --build build --target rv-boss                   # textured Mixamo character, CPU
cmake --build build --target benchmark-compute         # multi-pass dependency (Game of Life)
cmake --build build --target benchmark-diverge         # Mandelbrot/divergent whole pipelines
cmake --build build --target benchmark-compute-blur    # compute blur: GPU vs CPU throughput
cmake --build build --target cpu-scaling               # OpenMP scaling + Amdahl + RVV width
cmake --build build --target compare-backends          # static SPIR-V vs RISC-V comparison

bash test/script/run_packet_test.sh                    # SPMD packetizer regression
ctest --test-dir build -R benchmark_modes              # benchmark option/timing regressions
python3 test/script/bench_packet_pipeline.py           # separate native packet diagnostic
bash test/script/run_benchmark_compute.sh --sweep      # GPU/CPU crossover across grid sizes
bash test/script/run_cpu_scaling.sh --rvv-only         # RVV vector-width section only
```
