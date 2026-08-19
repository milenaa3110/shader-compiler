# Testing Guide

Command reference for building and running the tests. Each benchmark script's
header comment explains what it measures and why — this file is just the *how*.

## Prerequisites

```bash
# Core build (always required): CMake 3.20+, C++20 compiler, LLVM 18 toolchain
sudo apt install cmake build-essential llvm-18 clang-18

# Linked/included libs + SPIR-V opcode counter + MP4 output
sudo apt install libfmt-dev libvulkan-dev mesa-vulkan-drivers vulkan-tools \
    spirv-headers python3 ffmpeg

# Cross-compiling RISC-V from an x86 host (QEMU) — not needed on RISC-V hardware
sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu qemu-user-static
```

Notes:
- Vulkan + `spirv-headers` are checked at configure time even for non-GPU targets.
  `mesa-vulkan-drivers` supplies LavaPipe so GPU targets run without a discrete GPU.
- LLVM 17 is auto-detected as a fallback if `llvm-18` is absent.
- On native RISC-V hardware (e.g. Banana Pi F3): install only the Core build
  packages; scripts detect `uname -m == riscv64` and use the native toolchain —
  no QEMU, no cross-compiler. `.rv` binaries run directly and RVV executes on-core.

## Build

```bash
cmake -S . -B build                # one-time configure
cmake --build build -j$(nproc)     # build all tools + sincos_opt.so
cmake --build build --target clean # remove generated artifacts
```

Artifacts: `build/spirv/` (SPIR-V + Vulkan hosts), `build/riscv/` (`.ll`/`.o`/`.rv`),
`build/llvm/` (pass plugin). Render output (`.mp4`) lands in `result/`.
From inside `build/`, plain `make <target>` also works.

---

## Tests

### 1. Compiler unit tests
```bash
cmake --build build --target check          # compile every compiler_tests/*.src + llvm-as
ctest --test-dir build                      # run every correctness gate below
```

The correctness gates (each is a CTest case; run one directly with its script):

| Gate | Script | What it checks |
|------|--------|----------------|
| `lexer_test` | `build/lexer_test` | Unit test of the buffer-backed lexer |
| `sema_test` | `build/sema_test` | Type-system rules: result types + rejected constructs (no codegen) |
| `ir_checks` | `test/script/run_ir_checks.sh` | FileCheck over emitted IR shape (needs FileCheck-18) |
| `reject_tests` | `test/script/run_reject_tests.sh` | Shaders that must fail with an exact diagnostic; a crash is a failure |
| `matrix_numeric` | `test/script/run_matrix_numeric.sh` | Links emitted IR to a hand-computed C++ driver and compares column-major results (needs clang-18) |
| `sampler_numeric` | `test/script/run_sampler_numeric.sh` | Binds real per-face/slice/layer/texel data and checks sampled values on the RISC-V path (needs clang-18) |
| `packet_test` | `test/script/run_packet_test.sh` | Emits `@fs_packet` and greps for the width-agnostic vectorized IR shape (select/if-else/discard/loop) |
| `packet_runtime` | `test/script/run_packet_runtime.sh` | Runs a packet build under QEMU: VLEN banner smoke + build-width guard (marker≠runtime → loud abort) (needs cross-cc + QEMU) |
| `texlayer_rv` | `test/rv_host/rv_host_texlayer.cpp` | Renders a cube/3D/array/2D + image2D shader under QEMU, binding via `bind_texture_layered`/`bind_texture`/`bind_image`; checks the pixel + image round-trip (needs cross-cc + QEMU) |
| `compare_backends` | `test/script/compare_backends.sh` | Descriptive SPIR-V-vs-RISC-V static metrics table (bytes/opcodes/insns); no pass/fail (needs python3) |
| `spirv_validate` | `test/script/run_spirv_val.sh` | `spirv-val` over every emitted `.spv` (needs spirv-val) |

Matrix support is covered end-to-end across these: `sema_test` stamps the matrix
type rules, `matrix_numeric` verifies arithmetic / component-wise ops / builtins
(`transpose`, `inverse`, `determinant`, `matrixCompMult`, `outerProduct`) /
assignment / `mat++`, `reject_tests` pins the diagnostics, and `spirv_validate`
runs `matdemo.vert.spv` — the one shader that exercises the real `OpTypeMatrix`
path (ColMajor/MatrixStride uniforms, column indexing incl. a *direct* index on a
uniform matrix `uMVP[i]`, `mat == mat` in a bool context, and a matrix varying).

Sampler/image support spans all types on both backends: `sampler2D/3D/Cube/2DArray`
+ `texture`/`textureLod`, and `image2D`/`imageBuffer` + `imageLoad`/`imageStore`.
The SPIR-V backend dispatches per dimension (real `OpTypeImage` Cube/3D/2D-arrayed,
`OpImageSampleExplicitLod`, `OpImageRead`/`OpImageWrite`), covered by
`cubemap.frag.spv` and `imagestore.comp.spv` under `spirv_validate`; an unlowered
sampler/image intrinsic now fails the build loudly instead of writing an invalid
module. The RISC-V runtime (`src/runtime/tex_inline.cpp`) implements each intrinsic against
real bound data — cube-face selection, 3D trilinear, array-layer select, and
storage load/store, indexed by the sampler's binding slot. The `sampler_numeric`
CTest binds real per-face/slice/layer/texel data and checks the sampled values
(incl. an `imageStore`→`imageLoad` round-trip and that `textureLod` clamps to the
base level). The `texlayer_rv` CTest closes the render-pipeline loop: it binds a
cube/3D/array/2D texture and a read/write image2D via `bind_texture_layered`/
`bind_texture`/`bind_image` and renders `texlayer_fs` under QEMU, checking the
output pixel and the image round-trip end-to-end.

> **Known limitation — layered render shaders use reduced opt.** `opt -O3`
> miscompiles cube/3D/array/image sampling on the RISC-V backend: inlining the
> `always_inline` sampler bodies (which write `float* out`, read back as
> `<4 x float>` under `vscale_range`) into the shader yields a NULL-deref / wrong
> pixel (LLVM-18 backend bug, reproduces at `llc -O0`, scalar and vector). The
> runtime is correct — `sampler_numeric` and the x86 path pass, and a
> **non-inlining** opt pipeline renders bit-exactly. So the `texlayer_rv` shader
> object is built with a curated pipeline (mem2reg/sroa/gvn/dse/… — no inliner) +
> `llc -O3` (see the CMake comment). 2D-only render shaders keep the aggressive
> `-O3`; they don't hit the bug. Remaining follow-up: mip pyramids (`textureLod`
> clamps to the base level) and root-causing the `-O3` inliner miscompile upstream.

### 2. Vulkan GPU animations (single shader)
```bash
cmake --build build --target vk-mandelbrot   # 60 frames → result/mandelbrot.mp4
# also: vk-julia vk-voronoi vk-waves vk-tunnel vk-ripple vk-galaxy vk-fire
#       vk-reaction vk-cellular vk-earth vk-scene3d vk-diverge vk-city vk-ocean
cmake --build build --target all-vk          # build every Vulkan shader
```
Device selection: picks a real GPU, falls back to LavaPipe (stderr warning) when
none is present. Force a device with `VK_DEVICE_INDEX=<n>`.

Build-only terrain/texture demos, run manually:
```bash
cmake --build build --target terrain.vert.spv terrain.frag.spv spirv_vulkan_host
build/spirv/spirv_vulkan_host build/spirv/terrain.vert.spv build/spirv/terrain.frag.spv terrain 60 512 512

cmake --build build --target texture_test.frag.spv quad.vert.spv spirv_vulkan_texture_host
build/spirv/spirv_vulkan_texture_host build/spirv/quad.vert.spv build/spirv/texture_test.frag.spv texture_test 60 512 512
```

### 3. RISC-V CPU animations (same shaders, different backend)
```bash
cmake --build build --target rv-mandelbrot   # same names as vk-* for all animations
cmake --build build --target all-rv          # build every RISC-V binary
```
Build-only terrain/texture binaries, run directly:
```bash
cmake --build build --target terrain.rv
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu build/riscv/terrain.rv

cmake --build build --target texture_test.rv
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu build/riscv/texture_test.rv
```

### 4. GPU vs CPU fragment benchmark
```bash
cmake --build build --target benchmark-fragment        # all 14 shaders
cmake --build build --target benchmark-fragment-quick  # fewer frames

bash test/script/run_benchmark_fragment.sh [--quick|--rv-only|--vk-only]
```

### 5. Terrain vertex-shader benchmark
```bash
cmake --build build --target benchmark-vertex
bash test/script/run_benchmark_vertex.sh
```

### 5b. Indexed-mesh demo (VBO + IBO)
```bash
cmake --build build --target vk-mesh         # GPU,  1280 tris   (rv-mesh    = CPU)
cmake --build build --target vk-mesh-hi      # GPU, 20480 tris   (rv-mesh_hi = CPU)

# OBJ assets — same vk-/rv- naming:
#   vk-mesh-bunny / rv-bunny   Stanford bunny, 4968 tris
#   vk-mesh-jeep  / rv-jeep    textured vehicle, 4728 tris
#   vk-mesh-teddy / rv-teddy   high-poly teddy, 1.5M tris (CPU slow — minutes)
#   vk-mesh-boss  / rv-boss    textured Mixamo character, 10220 tris
```
Same source (`mesh_vs.src` + `mesh_fs.src`) for both backends; rendered at 768×768,
`rv-*` runs 300 frames. Vulkan host uses a negative-height viewport so GPU/CPU
output is pixel-comparable.

### 5c. Indexed-mesh benchmark
```bash
cmake --build build --target benchmark-mesh
bash test/script/run_benchmark_mesh.sh
```

### 6. Game of Life benchmark (multi-pass)
```bash
cmake --build build --target benchmark-compute

bash test/script/run_benchmark_compute.sh --tiny            # 32×32 only
bash test/script/run_benchmark_compute.sh --sweep           # grid sizes 16→512
bash test/script/run_benchmark_compute.sh --animate         # 600 gens as MP4
bash test/script/run_benchmark_compute.sh --grid 128 --gens 500
```

### 7. Branch-divergence benchmark
```bash
cmake --build build --target benchmark-diverge
bash test/script/run_benchmark_diverge.sh [--quick]
```
Section 1 times the CPU twice from the same binary — width-4 packet
(`SHADER_PACKET=1`) and scalar per-pixel (`SHADER_PACKET` unset) — so the
divergence efficiency of the two dispatch paths sits side by side with the GPU's.
`pkt gain` is scalar ÷ packet; under QEMU it drops below 1.0× (see the RVV note
in §11), so judge the packetizer with `bench_packet.sh` instead.

### 8. Compute shader benchmark (Gaussian blur)
```bash
cmake --build build --target benchmark-compute-blur
bash test/script/run_benchmark_compute_blur.sh
```

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
bash test/script/bench_packet.sh        # real SIMD speedup on host (~3.8×), sidesteps QEMU
```
The packet width is a single source of truth (`SHADER_PACKET_WIDTH`, default 4,
`src/common/packet_width.h`) baked into both `irgen_riscv` (`kW`) and the runtime
(`PACKET_W`). `irgen_riscv` stamps a `__shader_packet_width` marker into the shader
`.rv`; the runtime aborts loudly on a mismatch (the SoA stride would otherwise
silently corrupt every pixel). `-DSHADER_PACKET_WIDTH=auto` probes `vlenb` on a
native RISC-V build (`VLEN/32` f32 lanes); cross/QEMU builds default to 4.
Selected at runtime with `SHADER_PACKET=1` — set automatically by the `rv-*`
animation/mesh targets and the `benchmark-fragment/vertex/mesh/diverge` +
`cpu-scaling` targets (shaders outside the supported subset fall back to scalar).
Force scalar for an A/B run with `SHADER_PACKET= cmake --build build --target rv-mandelbrot`.

> QEMU emulates each RVV instruction with a scalar loop, so the CPU-side
> wall-clock in every RISC-V benchmark is **not** representative of real
> hardware. Judge the packet/RVV path by static instruction count (`objdump`)
> or the host measurement in `bench_packet.sh`. On a real RISC-V board the
> packet/RVV path is the faster one (~2–4× on float-heavy shaders).

### Verify RVV instructions are generated
```bash
cmake --build build --target mandelbrot_rv.o
riscv64-linux-gnu-objdump -d build/riscv/mandelbrot_rv.o | grep -E 'vl[ew]|vf(add|mul|sub|div)|vset'
```

---

## Quick reference

```bash
cmake --build build --target check                     # compiler correctness (unit tests)
cmake --build build --target vk-mandelbrot             # single GPU animation
cmake --build build --target rv-mandelbrot             # same shader on RISC-V
cmake --build build --target all-vk                    # build every Vulkan shader
cmake --build build --target all-rv                    # build every RISC-V shader
cmake --build build --target benchmark-fragment-quick  # GPU vs CPU, 14 fragment shaders
cmake --build build --target benchmark-vertex          # terrain vertex shader: GPU vs CPU
cmake --build build --target benchmark-mesh            # textured indexed-mesh: GPU vs CPU
cmake --build build --target vk-mesh                   # indexed icosphere (1280 tris), GPU
cmake --build build --target rv-mesh                   # indexed icosphere (1280 tris), CPU
cmake --build build --target vk-mesh-boss              # textured Mixamo character, GPU
cmake --build build --target rv-boss                   # textured Mixamo character, CPU
cmake --build build --target benchmark-compute         # multi-pass dependency (Game of Life)
cmake --build build --target benchmark-diverge         # branch divergence + warp boundary
cmake --build build --target benchmark-compute-blur    # compute blur: GPU vs CPU throughput
cmake --build build --target cpu-scaling               # OpenMP scaling + Amdahl + RVV width
cmake --build build --target compare-backends          # static SPIR-V vs RISC-V comparison

bash test/script/run_packet_test.sh                    # SPMD packetizer regression
bash test/script/bench_packet.sh                       # SPMD packet vs scalar speedup (host SIMD)
bash test/script/run_benchmark_compute.sh --sweep      # GPU/CPU crossover across grid sizes
bash test/script/run_cpu_scaling.sh --rvv-only         # RVV vector-width section only
```
