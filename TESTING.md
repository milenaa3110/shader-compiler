# Testing Guide

## Prerequisites

```bash
# LLVM 18 toolchain
sudo apt install llvm-18 clang-18

# RISC-V cross-compiler + QEMU user-mode emulation
sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu qemu-user-static

# Vulkan SDK (for GPU tests)
sudo apt install libvulkan-dev glslang-tools mesa-vulkan-drivers

# ffmpeg (for MP4 animation output)
sudo apt install ffmpeg
```

## Build

```bash
make            # build compiler tools: irgen_riscv, irgen_spirv, shader_codegen, sincos_opt.so
make clean      # remove all generated artifacts (build/ and result/)
```

All intermediate artifacts (`.ll`, `.o`, `.rv`, `.spv`) land in `build/`.
All render output (`.mp4`) lands in `result/`.

---

## Test Categories

### 1. Compiler Unit Tests
```bash
make check           # run test/run_tests.sh (parser + codegen correctness)
make check-verbose   # same with per-test output
```
**Goal:** Verify the GLSL→LLVM IR compiler is correct — lexing, parsing, type checking,
and code generation. Each test shader is compiled with `irgen_riscv` and the resulting
LLVM IR is validated with `llvm-as`.

---

### 2. Vulkan GPU Animations (single shader)
```bash
make vk-mandelbrot    # renders 60 frames → result/mandelbrot.mp4
make vk-julia         # same for julia set
make vk-voronoi
make vk-waves
make vk-tunnel
make vk-ripple
make vk-galaxy
make vk-fire
make vk-reaction      # reaction-diffusion
make vk-cellular      # cellular automata
make vk-earth
make vk-scene3d       # 3-D SDF raymarcher with soft shadows + AO
make vk-diverge       # branch-divergence test shader
make vk-texture       # texture sampling: procedural marble texture, 9-tap UV distortion
make all-vk           # run all animations sequentially
```
**Goal:** Verify the SPIR-V/Vulkan pipeline works end-to-end. Each shader is a
self-contained fragment shader running on the GPU via LavaPipe (software Vulkan) or
real hardware.

---

### 3. RISC-V CPU Animations (same shaders, different backend)
```bash
make rv-mandelbrot    # compiles via irgen_riscv + llc-18, runs on QEMU
make rv-julia
# ... same names as vk-* for all animations
make rv-texture_test  # texture sampling: software bilinear sampler in pipeline_runtime.cpp
make all-rv           # run all animations on RISC-V
```
**Goal:** Verify the RISC-V backend produces correct output. The same shader compiled to
RV64GCV machine code and run under QEMU with OpenMP threading.

The texture test specifically exercises the software bilinear sampler (`__tex_lookup` in
`pipeline_runtime.cpp`). It uses 9 UV-distorted taps per pixel — the same shader as
`vk-texture` but running on the CPU, demonstrating the TMU vs software sampler tradeoff.

---

### 4. GPU vs CPU Benchmark (main comparison)
```bash
make benchmark-fragment        # full run: all 12 shaders, 7-column table
make benchmark-fragment-quick  # fewer frames, faster

# Or directly:
bash test/run_benchmark_fragment.sh
bash test/run_benchmark_fragment.sh --quick
bash test/run_benchmark_fragment.sh --rv-only    # skip Vulkan
bash test/run_benchmark_fragment.sh --vk-only    # skip RISC-V
```
**Goal:** Compare GPU (Vulkan SPIR-V) vs CPU (RISC-V + OpenMP via QEMU) for 12 fragment
shaders. Measures ms/frame, speedup factor, SPIR-V binary size, RV object size, and
instruction count. GPU wins on all fragment shaders — this is expected and demonstrates
why GPUs exist.

---

### 5. Game of Life Benchmark (multi-pass dependency)
```bash
make benchmark-compute         # 32×32 CPU-wins case + 256×256 main run
make benchmark-compute-sweep   # grid sizes 16→512, shows GPU/CPU crossover point
make benchmark-compute-animate # records 600 generations as MP4 (GPU + CPU)

bash test/run_benchmark_compute.sh --tiny    # 32×32 only
bash test/run_benchmark_compute.sh --sweep   # crossover table
bash test/run_benchmark_compute.sh --animate # MP4 output
bash test/run_benchmark_compute.sh --grid 128 --gens 500
```
**Goal:** Show that GPU advantage is not universal. Game of Life requires N sequential
dispatches (one per generation) — each carrying ~0.15 ms of `vkQueueSubmit` overhead.
At small grids (≤32×32) this overhead exceeds compute time and the CPU wins.
**Crossover:** GPU wins above ~64×64, CPU wins below.

---

### 6. Branch Divergence Benchmark
```bash
make benchmark-diverge        # full 3-section analysis
make benchmark-diverge-quick  # faster run

bash test/run_benchmark_diverge.sh
bash test/run_benchmark_diverge.sh --quick
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

### 7. Compute Shader Benchmark (Gaussian blur)
```bash
make benchmark-compute-blur
bash test/run_benchmark_compute_blur.sh
```
**Goal:** GPU Vulkan compute (`blur.comp`, 5×5 Gaussian, 16×16 workgroups) vs CPU
RISC-V OpenMP blur on the same 512×512 data. Shows raw data-parallel throughput
(Mpixels/ms). GPU wins by ~40–50× here — compute shaders are the GPU's strongest case.

---

### 8. CPU Thread Scaling + RVV Vector Width Analysis
```bash
make cpu-scaling        # full 5-section analysis (~10 min)
make cpu-scaling-quick  # fast version (skips sections 1–4 long runs)
make bench-rvv-width    # section 5 only: RVV vector width test

bash test/run_cpu_scaling.sh
bash test/run_cpu_scaling.sh --quick
bash test/run_cpu_scaling.sh --rvv-only
```
Five analyses on RISC-V + OpenMP via QEMU:
1. **Thread scaling** — mandelbrot and diverge shaders at 1/2/4/8 threads. Speedup,
   efficiency, and Amdahl prediction.
2. **Amdahl fit** — estimates serial fraction from T(1) and T(max). Identifies the
   rasterizer overhead (~12%) as the scaling ceiling for fragment shaders.
3. **Cache-size effect** — Game of Life at 32×32 (L1), 128×128 (L2), 256×256 (L3+).
   Smaller grids can anti-scale at high thread counts due to QEMU JIT + OpenMP overhead.
4. **Static vs dynamic scheduling** — OpenMP `schedule(static)` vs `schedule(dynamic,4)`
   for mandelbrot and diverge. Diverge's vertical split balances each row equally, so
   dynamic scheduling adds overhead with negligible gain.
5. **RVV vector width scaling** — three sub-tests:
   - **Runtime VLEN probe** — compiles a `csrr vlenb` binary; runs under QEMU with
     `-cpu rv64,v=true,vlen=128/256/512` to confirm which VLEN QEMU is simulating.
   - **Timing benchmark** — mandelbrot rendered at VLEN=128/256/512. QEMU wall-clock
     gain is small (simulates each vector op 1:1), but shows the mechanism. On real
     RVV hardware expect ~2× (256-bit) / ~4× (512-bit) speedup for float-heavy loops.
   - **Scalar vs. vector FP ops** — recompiles `mandelbrot_rv.ll` with and without
     `+v` and counts scalar `fadd.s`/`fmul.s` vs. vector `vfadd.vv`/`vfmul.vv` ops
     via `objdump`. Each vector op replaces N scalar ops (N = VLEN/32). Total
     instruction count stays similar due to `vsetvli`/`vle`/`vse` overhead, but each
     vector FP instruction does 4–16× more float work depending on hardware VLEN.

---

## RISC-V Vector Extension (RVV)

### What is already enabled

The project compiles RISC-V shaders with `-mattr=+m,+a,+f,+d,+v`, where `+v` activates
the **RVV (RISC-V Vector) extension**. LLVM's auto-vectorizer uses this to generate
vector instructions (`vle32`, `vfadd`, `vfmul`, etc.) for inner loops in shaders and
the `pipeline_runtime.cpp` pixel rasterizer.

### Verify that vector instructions are being generated

```bash
# Build mandelbrot RISC-V object
make build/mandelbrot_rv.o

# Check for RVV instructions in the compiled shader
riscv64-linux-gnu-objdump -d build/mandelbrot_rv.o | grep -E 'vl[ew]|vf(add|mul|sub|div)|vset' | head -20

# Count vector vs scalar FP instructions
echo "Vector FP:"; riscv64-linux-gnu-objdump -d build/mandelbrot_rv.o | grep -cE 'vf(add|mul|sub|div)' || true
echo "Scalar FP:"; riscv64-linux-gnu-objdump -d build/mandelbrot_rv.o | grep -cE '\bf(add|mul|sub|div)\.d\b' || true
```

### Measure the RVV speedup

Compare the same shader compiled with and without the V extension:

```bash
# Without RVV (scalar only)
llc-18 -O3 -filetype=obj -relocation-model=pic \
    -mtriple=riscv64-unknown-linux-gnu \
    -mattr=+m,+a,+f,+d \
    build/mandelbrot_rv.ll -o build/mandelbrot_scalar.o

riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp -Ipipeline \
    -DANIM_NAME='"mandelbrot"' -DNFRAMES=8 -DWIDTH=256 -DHEIGHT=256 \
    test/rv_host/rv_host_fragment.cpp pipeline/pipeline_runtime.cpp \
    build/mandelbrot_scalar.o -o build/mandelbrot_scalar.rv

# With RVV (default — already in Makefile)
make build/mandelbrot.rv

# Run both and compare
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu ./build/mandelbrot_scalar.rv
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu ./build/mandelbrot.rv
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
# With RVV
echo "With RVV:"; riscv64-linux-gnu-objdump -d build/mandelbrot_rv.o | grep -cE '^\s+[0-9a-f]+:'
# Without RVV
echo "Without:"; riscv64-linux-gnu-objdump -d build/mandelbrot_scalar.o | grep -cE '^\s+[0-9a-f]+:'
```

---

## Quick Reference

| Command | What it tests |
|---------|---------------|
| `make check` | Compiler correctness (unit tests) |
| `make vk-mandelbrot` | Single GPU animation |
| `make rv-mandelbrot` | Same shader on RISC-V |
| `make vk-texture` | GPU texture sampling (hardware TMU, 9 taps) |
| `make rv-texture_test` | CPU texture sampling (software bilinear, same shader) |
| `make benchmark-fragment-quick` | GPU vs CPU across 12 shaders |
| `make benchmark-compute` | Multi-pass dependency (GPU dispatch overhead) |
| `make benchmark-compute-sweep` | Crossover point: small grid CPU wins |
| `make benchmark-diverge` | Branch divergence + warp boundary effect |
| `make benchmark-compute-blur` | Compute shader (blur): GPU vs CPU throughput |
| `make cpu-scaling` | OpenMP thread scaling + Amdahl law fit + RVV width |
| `make bench-rvv-width` | RVV vector width only (VLEN=128/256/512) |
| `make benchmark-diverge-quick` | Quick divergence demo |
