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
cmake --build build --target check
```

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

### 11. SPMD packetizer (width-4 SIMD)
```bash
bash test/script/run_packet_test.sh   # emit + per-lane equivalence + bit-identical render
bash test/script/bench_packet.sh      # real SIMD speedup on host (~3.8×), sidesteps QEMU
```
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
