# Testing Guide

## Prerequisites

```bash
# CMake 3.20+ and a C++20 compiler
sudo apt install cmake build-essential

# LLVM 18 toolchain (LLVM 17 is auto-detected as a fallback)
sudo apt install llvm-18 clang-18 libfmt-dev

# RISC-V cross-compiler + QEMU user-mode emulation
sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu qemu-user-static

# Vulkan SDK (for GPU tests) + SPIR-V headers
sudo apt install libvulkan-dev glslang-tools mesa-vulkan-drivers spirv-headers

# ffmpeg (for MP4 animation output)
sudo apt install ffmpeg
```

## Build

```bash
cmake -S . -B build                # one-time configure
cmake --build build -j$(nproc)     # build all compiler tools + sincos_opt.so
cmake --build build --target clean # remove generated artifacts
```

Intermediate artifacts land under the build tree:
- `build/spirv/` — SPIR-V binaries and Vulkan host executables
- `build/riscv/` — RISC-V `.ll`, `.o`, and `.rv` binaries
- `build/llvm/` — `sincos_opt.so` LLVM pass plugin

Render output (`.mp4`) lands in `result/`.

> The CMake-generated Makefile lets you also run targets from inside `build/` with plain
> `make`, e.g. `cd build && make vk-mandelbrot` is equivalent to
> `cmake --build build --target vk-mandelbrot`.

---

## Test Categories

### 1. Compiler Unit Tests
```bash
cmake --build build --target check
```
**Goal:** Verify the GLSL→LLVM IR compiler is correct — lexing, parsing, type checking,
and code generation. Each test shader is compiled with `irgen_riscv` and the resulting
LLVM IR is validated with `llvm-as`.

---

### 2. Vulkan GPU Animations (single shader)
```bash
cmake --build build --target vk-mandelbrot   # renders 60 frames → result/mandelbrot.mp4
cmake --build build --target vk-julia
# ... vk-voronoi, vk-waves, vk-tunnel, vk-ripple, vk-galaxy, vk-fire,
#     vk-reaction, vk-cellular, vk-earth, vk-scene3d, vk-diverge, vk-city, vk-ocean
cmake --build build --target all-vk          # build every Vulkan shader
```
**Goal:** Verify the SPIR-V/Vulkan pipeline works end-to-end. Each shader is a
self-contained fragment shader running on the GPU via LavaPipe (software Vulkan) or
real hardware.

The terrain mesh demo and the texture-sampling demo are build-only targets in CMake —
their artifacts (`terrain.vert.spv`, `terrain.frag.spv`, `texture_test.frag.spv`) can be
built and run manually with the appropriate Vulkan host:

```bash
cmake --build build --target terrain.vert.spv terrain.frag.spv spirv_vulkan_host
build/spirv/spirv_vulkan_host build/spirv/terrain.vert.spv build/spirv/terrain.frag.spv terrain 60 512 512

cmake --build build --target texture_test.frag.spv quad.vert.spv spirv_vulkan_texture_host
build/spirv/spirv_vulkan_texture_host build/spirv/quad.vert.spv build/spirv/texture_test.frag.spv texture_test 60 512 512
```

---

### 3. RISC-V CPU Animations (same shaders, different backend)
```bash
cmake --build build --target rv-mandelbrot   # compiles via irgen_riscv + llc-18, runs on QEMU
cmake --build build --target rv-julia
# ... same names as vk-* for all animations in ANIMATIONS_PROC
cmake --build build --target all-rv          # build every RISC-V binary
```
**Goal:** Verify the RISC-V backend produces correct output. The same shader compiled to
RV64GCV machine code and run under QEMU with OpenMP threading.

The terrain (`terrain.rv`) and texture-test (`texture_test.rv`) binaries are build-only
targets. Run them directly:

```bash
cmake --build build --target terrain.rv
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu build/riscv/terrain.rv

cmake --build build --target texture_test.rv
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu build/riscv/texture_test.rv
```

The texture test specifically exercises the software bilinear sampler (`__tex_lookup` in
`pipeline_runtime.cpp`). It uses 9 UV-distorted taps per pixel — the same shader as the
GPU `texture_test` but running on the CPU, demonstrating the TMU vs software sampler tradeoff.

---

### 4. GPU vs CPU Benchmark (main comparison)
```bash
cmake --build build --target benchmark-fragment        # full run: all 12 shaders, 7-column table
cmake --build build --target benchmark-fragment-quick  # fewer frames, faster

# Or directly:
bash test/script/run_benchmark_fragment.sh
bash test/script/run_benchmark_fragment.sh --quick
bash test/script/run_benchmark_fragment.sh --rv-only   # skip Vulkan
bash test/script/run_benchmark_fragment.sh --vk-only   # skip RISC-V
```
**Goal:** Compare GPU (Vulkan SPIR-V) vs CPU (RISC-V + OpenMP via QEMU) for 12 fragment
shaders. Measures ms/frame, speedup factor, SPIR-V binary size, RV object size, and
instruction count. GPU wins on all fragment shaders — this is expected and demonstrates
why GPUs exist.

---

### 5. Terrain Vertex Shader Benchmark
```bash
cmake --build build --target benchmark-vertex
bash test/script/run_benchmark_vertex.sh
```
**Goal:** Compare GPU (Vulkan) vs CPU (RISC-V + OpenMP) for a vertex-heavy workload —
a 32×32 animated terrain mesh (6144 vertices, all positions computed from `gl_VertexID`
with no vertex buffer). Measures ms/frame, VS/FS binary sizes, and RISC-V object size.
The two vertex shaders differ only in NDC conventions: `terrain_vs.src` for RISC-V
(standard Y), `terrain_vs_vk.src` for Vulkan (Y negated, z in [0,1]).

---

### 5b. Indexed-Mesh Demo (VBO + IBO, real geometry)
```bash
# Procedural icosphere (no external assets)
cmake --build build --target vk-mesh         # GPU,  1280 tris
cmake --build build --target vk-mesh-hi      # GPU,  20480 tris
cmake --build build --target rv-mesh         # CPU,  1280 tris
cmake --build build --target rv-mesh_hi      # CPU,  20480 tris

# OBJ assets — same vk-/rv- naming pattern
cmake --build build --target vk-mesh-bunny   / rv-bunny   # Stanford bunny, 4968 tris, no MTL
cmake --build build --target vk-mesh-jeep    / rv-jeep    # textured vehicle, 4728 tris
cmake --build build --target vk-mesh-teddy   / rv-teddy   # high-poly teddy, 1.5M tris (CPU is slow — minutes)
cmake --build build --target vk-mesh-boss    / rv-boss    # textured Mixamo character, 10220 tris
```
Same shader source (`mesh_vs.src` + `mesh_fs.src`) compiles to both backends.
The Vulkan host uses a negative-height viewport to handle Vulkan's flipped Y NDC,
so the GPU/CPU outputs are pixel-for-pixel comparable in framing — useful for
visual diffing when tweaking the projection or lighting math. Mesh targets render
at 768×768; the `rv-*` run targets render 300 frames.

The demo exercises:
- Vertex input attributes (`in vec3 aPos`, `in vec3 aNormal`, `in vec2 aUV`) with `Location 0/1/2`
- VBO + IBO upload via staging buffer (Vulkan) and direct memory feed (CPU)
- Indexed `vkCmdDrawIndexed` and the corresponding rasterizer index loop
- Per-material draw ranges (`usemtl`) with per-range `map_Kd` texture binding
- The software bilinear sampler (`tex_inline.cpp`, `llvm-link`'d into the shader)
- The unified `vs_invoke(vid, iid, flat_in, flat_out)` ABI on the RISC-V side
- The tile-based rasterizer's scaling on the 1.5M-tri teddy

The OBJ loader (`test/vk_host/obj_loader.h`) parses `v`/`vn`/`vt`/`f`, `mtllib`/
`usemtl`, and `map_Kd` diffuse maps; missing normals are computed by averaging
face normals. The icosphere generator (`test/vk_host/icosphere.h`) gives
N-subdivision control for triangle-count sweeps.

---

### 5c. Indexed-Mesh Benchmark (textured VBO + IBO pipeline)
```bash
cmake --build build --target benchmark-mesh
bash test/script/run_benchmark_mesh.sh
```
**Goal:** Compare GPU (Vulkan) vs CPU (RISC-V + OpenMP) for the full indexed-mesh
path — vertex buffers, index buffers, per-material textured draws, depth testing —
on the textured "boss" model (10220 tris / 30660 verts, 768×768, 60 frames).
Measures ms/frame, SPIR-V VS/FS sizes, RISC-V object size, and instruction count.
Under QEMU the CPU runs ~40× slower: emulation overhead plus software-rasterising
a textured mesh against a hardware triangle pipeline.

---

### 6. Game of Life Benchmark (multi-pass dependency)
```bash
cmake --build build --target benchmark-compute  # 32×32 CPU-wins case + 256×256 main run

# Direct script invocations expose more modes:
bash test/script/run_benchmark_compute.sh --tiny             # 32×32 only
bash test/script/run_benchmark_compute.sh --sweep            # grid sizes 16→512, GPU/CPU crossover
bash test/script/run_benchmark_compute.sh --animate          # 600 generations as MP4
bash test/script/run_benchmark_compute.sh --grid 128 --gens 500
```
**Goal:** Show how GPU/CPU balance shifts with grid size and generation count. GPU submits
all generations in one command buffer via pipeline barriers (no per-generation roundtrip).
Under QEMU, GPU wins at all grid sizes due to emulation overhead on the CPU side.
On real RISC-V hardware, small grids (≤32×32, fits in L1 cache, low GPU occupancy) would favour CPU.

---

### 7. Branch Divergence Benchmark
```bash
cmake --build build --target benchmark-diverge
bash test/script/run_benchmark_diverge.sh
bash test/script/run_benchmark_diverge.sh --quick
```
Three measurements:
1. **Divergence penalty** — `diverge.frag` (50/50 light/heavy pixels) vs `mandelbrot.frag`
   (uniform). GPU efficiency drops to ~50% because warps straddling x=0.5 execute all
   iterations for every lane. CPU threads run each branch independently → ~100% efficiency.
2. **Dispatch overhead isolation** — 1×1 pixel render isolates the fixed `vkQueueSubmit`
   cost (~0.15–0.35 ms/frame regardless of pixel count).
3. **Warp boundary dilution** — same diverge shader at 64/256/512 resolution shows the
   penalty shrinks as boundary warps become a smaller fraction of total pixels.

---

### 8. Compute Shader Benchmark (Gaussian blur)
```bash
cmake --build build --target benchmark-compute-blur
bash test/script/run_benchmark_compute_blur.sh
```
**Goal:** GPU Vulkan compute (`blur.comp`, 5×5 Gaussian, 16×16 workgroups) vs CPU
RISC-V OpenMP blur on the same 512×512 data. Shows raw data-parallel throughput
(Mpixels/ms). GPU wins by ~40–50× here — compute shaders are the GPU's strongest case.

---

### 9. CPU Thread Scaling + RVV Vector Width Analysis
```bash
cmake --build build --target cpu-scaling   # full 5-section analysis (~10 min)

bash test/script/run_cpu_scaling.sh
bash test/script/run_cpu_scaling.sh --quick      # fewer frames/generations
bash test/script/run_cpu_scaling.sh --rvv-only   # skip sections 1–4, run section 5
```
Five analyses on RISC-V + OpenMP via QEMU:
1. **Thread scaling** — mandelbrot and diverge shaders at 1/2/4/8 threads. Speedup,
   efficiency, and Amdahl prediction.
2. **Amdahl fit** — estimates serial fraction from T(1) and T(max). Identifies the
   rasterizer overhead (~12%) as the scaling ceiling for fragment shaders.
3. **Cache-size effect** — Game of Life at 32×32 (L1), 128×128 (L2), 256×256 (L3+).
   Smaller grids can anti-scale at high thread counts due to QEMU JIT + OpenMP overhead.
4. **Static vs dynamic scheduling** — OpenMP `schedule(static)` vs `schedule(dynamic,1)`
   for mandelbrot and diverge. Diverge's vertical split balances each row equally, so
   dynamic scheduling adds overhead with negligible gain.
5. **RVV vector width scaling** — three sub-tests:
   - **Runtime VLEN probe** — compiles a `csrr vlenb` binary; runs under QEMU with
     `-cpu rv64,v=true,vlen=128/256/512` to confirm which VLEN QEMU is simulating.
   - **Timing benchmark** — mandelbrot rendered at VLEN=128/256/512. QEMU wall-clock
     gain is small (simulates each vector op 1:1), but shows the mechanism. On real
     RVV hardware expect ~2× (256-bit) / ~4× (512-bit) speedup for float-heavy loops.
   - **Scalar vs. vector FP ops** — takes `build/riscv/blur_cs_rv.opt.ll` (the actual
     Gaussian blur compute shader IR after opt) and compiles it twice with `llc-18`:
     once with `+v` (RVV enabled) and once without (scalar only, `+v` stripped from
     function attributes). Counts and displays sample instructions from `cs_dispatch_row`
     in each variant. With RVV: `vfadd.vv`, `vfmul.vf`, `vfmadd.vf` etc.; without:
     `fadd.s`, `fmul.s` etc. The X-loop in `cs_dispatch_row` auto-vectorizes because it
     has a fixed iteration count and no cross-iteration FP dependencies. Each vector FP
     instruction processes VLEN/32 floats — 4× at VLEN=128, 8× at VLEN=256, 16× at VLEN=512.

---

## RISC-V Vector Extension (RVV)

### What is already enabled

The project compiles RISC-V shaders with `-mattr=+m,+a,+f,+d,+v`, where `+v` activates
the **RVV (RISC-V Vector) extension**. LLVM's auto-vectorizer uses this to generate
vector instructions (`vle32`, `vfadd`, `vfmul`, etc.) for inner loops in the compiled shader.
`pipeline_runtime.cpp` (the software rasterizer) is cross-compiled with `-march=rv64gcv`,
enabling GCC to auto-vectorize fixed-count loops such as the varying interpolation inner loop.

### Verify that vector instructions are being generated

```bash
# Build mandelbrot RISC-V object
cmake --build build --target mandelbrot_rv.o

# Check for RVV instructions in the compiled shader
riscv64-linux-gnu-objdump -d build/riscv/mandelbrot_rv.o | grep -E 'vl[ew]|vf(add|mul|sub|div)|vset' | head -20

# Count vector vs scalar FP instructions
echo "Vector FP:"; riscv64-linux-gnu-objdump -d build/riscv/mandelbrot_rv.o | grep -cE 'vf(add|mul|sub|div)' || true
echo "Scalar FP:"; riscv64-linux-gnu-objdump -d build/riscv/mandelbrot_rv.o | grep -cE '\bf(add|mul|sub|div)\.s\b' || true
```

### Measure the RVV speedup

Compare the same shader compiled with and without the V extension:

```bash
# Without RVV (scalar only)
cmake --build build --target mandelbrot_rv.ll
llc-18 -O3 -filetype=obj -relocation-model=pic \
    -mtriple=riscv64-unknown-linux-gnu \
    -mattr=+m,+a,+f,+d \
    build/riscv/mandelbrot_rv.ll -o build/riscv/mandelbrot_scalar.o

riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp -Ipipeline \
    -DANIM_NAME='"mandelbrot"' -DNFRAMES=8 -DWIDTH=256 -DHEIGHT=256 \
    test/rv_host/rv_host_fragment.cpp pipeline/pipeline_runtime.cpp \
    build/riscv/mandelbrot_scalar.o -o build/riscv/mandelbrot_scalar.rv

# With RVV (default)
cmake --build build --target mandelbrot.rv

# Run both and compare
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu ./build/riscv/mandelbrot_scalar.rv
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu ./build/riscv/mandelbrot.rv
```

### Why RVV matters for this project

| Aspect | Benefit |
|--------|---------|
| **Fragment shaders** | Inner color-computation loops (sin/cos/multiply chains) vectorize over multiple pixels per thread |
| **Game of Life** | The 8-neighbor sum loop across a row can be vectorized with integer gather/add |
| **Compute blur** | The 5×5 convolution inner loop is a natural SIMD reduction |
| **Instruction count** | RVV should reduce instruction count in `rv_instr_count()` output by 2–8× for math-heavy shaders |
| **Academic value** | Demonstrates the compiler emitting modern RVV code via LLVM; contrast with GPU SIMT model |

### Expected observation

QEMU does not accelerate RVV instructions (it simulates them 1:1), so wall-clock time
under QEMU may not improve or could worsen. The meaningful metric is **static instruction
count** (from `objdump`) — fewer instructions = more work done per clock cycle on real
hardware. On a real RISC-V board (e.g. StarFive VisionFive 2, Milk-V Pioneer) RVV would
show a real speedup of 2–4× on float-heavy shaders.

To see instruction count difference:
```bash
echo "With RVV:"; riscv64-linux-gnu-objdump -d build/riscv/mandelbrot_rv.o | grep -cE '^\s+[0-9a-f]+:'
echo "Without:"; riscv64-linux-gnu-objdump -d build/riscv/mandelbrot_scalar.o | grep -cE '^\s+[0-9a-f]+:'
```

---

## Quick Reference

All commands assume the project root and a configured build dir (`cmake -S . -B build`).

| Command | What it tests |
|---------|---------------|
| `cmake --build build --target check` | Compiler correctness (unit tests) |
| `cmake --build build --target vk-mandelbrot` | Single GPU animation |
| `cmake --build build --target rv-mandelbrot` | Same shader on RISC-V |
| `cmake --build build --target all-vk` | Build every Vulkan shader |
| `cmake --build build --target all-rv` | Build every RISC-V binary |
| `cmake --build build --target benchmark-fragment-quick` | GPU vs CPU across 12 fragment shaders |
| `cmake --build build --target benchmark-vertex` | Terrain vertex shader: GPU vs CPU |
| `cmake --build build --target benchmark-mesh` | Textured indexed-mesh pipeline: GPU vs CPU |
| `cmake --build build --target vk-mesh` / `rv-mesh` | Indexed icosphere mesh (1280 tris) — GPU/CPU |
| `cmake --build build --target vk-mesh-bunny` / `rv-bunny` | Stanford bunny mesh — GPU/CPU |
| `cmake --build build --target vk-mesh-boss` / `rv-boss` | Textured Mixamo character mesh — GPU/CPU |
| `cmake --build build --target benchmark-compute` | Multi-pass dependency (GPU dispatch overhead) |
| `cmake --build build --target benchmark-diverge` | Branch divergence + warp boundary effect |
| `cmake --build build --target benchmark-compute-blur` | Compute shader (blur): GPU vs CPU throughput |
| `cmake --build build --target cpu-scaling` | OpenMP thread scaling + Amdahl law fit + RVV width |
| `bash test/script/run_benchmark_compute.sh --sweep` | Crossover point: small grid CPU wins |
| `bash test/script/run_benchmark_compute.sh --animate` | Game of Life MP4 (GPU + CPU) |
| `bash test/script/run_cpu_scaling.sh --rvv-only` | RVV vector width section only |
| `bash test/script/run_benchmark_diverge.sh --quick` | Quick divergence demo |
