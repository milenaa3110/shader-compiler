# GLSL Compiler — RISC-V + Vulkan SPIR-V

A research compiler for a GLSL-inspired language that compiles shaders to two targets:
- **RISC-V LLVM IR** — runs on CPU via QEMU, parallelised with OpenMP, with optional RVV (RISC-V Vector Extension) auto-vectorisation
- **Vulkan SPIR-V** — runs on GPU via the Vulkan API (LavaPipe software renderer or real GPU)

The primary goal is to compare GPU and CPU execution of the same shader logic, measure GPU dispatch overhead, observe OpenMP thread scaling, and demonstrate RVV vectorisation.

---

## Project structure

```
.
├── lexer/              Tokeniser
├── parser/             Recursive-descent parser → AST
├── ast/                AST node definitions
├── codegen_state/      Codegen context and symbol table
├── main/               irgen_riscv and irgen_spirv entry points
├── main_codegen/       shader_codegen (interactive IR dump tool)
├── helpers/            call_helpers, assignment_helpers
├── passes/             sincos_opt.cpp — LLVM pass plugin
├── pipeline/           Software rasterizer (pipeline_runtime.cpp/h, pipeline_abi.h)
├── test/
│   ├── rv_host/        RISC-V benchmark hosts (cross-compiled, run under QEMU)
│   │   ├── rv_host_fragment.cpp      generic animation host
│   │   ├── rv_host_compute.cpp       Game of Life CPU host
│   │   └── rv_host_compute_blur.cpp  Gaussian blur CPU host
│   ├── vk_host/        Vulkan host programs (run on the host, drive the GPU)
│   │   ├── vk_host_fragment.cpp      offscreen animation renderer
│   │   ├── vk_host_compute.cpp       Game of Life Vulkan host
│   │   ├── vk_host_compute_blur.cpp  Gaussian blur Vulkan host
│   │   └── vk_host_texture.cpp       texture sampling Vulkan host
│   ├── script/         Shell scripts (benchmarks, tests)
│   └── shaders/
│       ├── animations/   Fragment + vertex + compute shader sources (.src, .comp)
│       ├── pipeline/     Pipeline test shaders
│       └── compiler_tests/  Compiler unit test shaders
└── build/
    ├── riscv/          irgen_riscv binary, .ll / .o / .rv intermediates
    ├── spirv/          irgen_spirv binary, Vulkan hosts, .spv bytecode
    └── llvm/           sincos_opt.so, compiler object files
```

---

## Dependencies

```bash
# LLVM 18
sudo apt install llvm-18 clang-18

# RISC-V cross-compiler and QEMU user-mode emulation
sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu qemu-user-static

# Vulkan (GPU tests, uses LavaPipe software renderer if no GPU)
sudo apt install libvulkan-dev glslang-tools mesa-vulkan-drivers

# ffmpeg (MP4 output)
sudo apt install ffmpeg
```

---

## Build

```bash
make          # builds: build/riscv/irgen_riscv, build/spirv/irgen_spirv,
              #         build/shader_codegen, build/llvm/sincos_opt.so
make clean    # removes build/ and result/
```

---

## How a shader becomes a video

```
shader_fs.src
  │
  ├─ irgen_riscv → vs.ll + fs.ll → llvm-link → opt -O3 → sincos_opt.so → llc → .o
  │                                                                              │
  │                                             rv_host_fragment.cpp + pipeline_runtime.cpp
  │                                                                              │
  │                                                          riscv64 ELF (.rv)
  │                                                                              │
  │                                                      QEMU + OpenMP → frames → MP4
  │
  └─ irgen_spirv → GLSL 450 → glslangValidator → .spv
                                                    │
                              vk_host_fragment + Vulkan API (LavaPipe) → frames → MP4
```

The sincos pass fuses `sin(x) + cos(x)` pairs into a single `sincosf(x, &s, &c)` call after `opt -O3` has unified duplicate arguments via GVN.

---

## Make targets

### Compiler unit tests
```bash
make check            # compile all test shaders, validate IR with llvm-as
make check-verbose    # same with per-test output
```

### Single-shader Vulkan animations
```bash
make vk-mandelbrot    # renders 60 frames → result/mandelbrot.mp4
make vk-julia
make vk-voronoi
make vk-waves
make vk-tunnel
make vk-fire
make vk-galaxy
make vk-ripple
make vk-reaction
make vk-cellular
make vk-earth
make vk-scene3d
make vk-texture       # texture sampling demo
make vk-terrain       # vertex shader — procedural terrain mesh
make all-vk           # all 13 fragment animations
```

### Single-shader RISC-V animations
```bash
make rv-mandelbrot    # renders 60 frames via QEMU → result/mandelbrot_rv.mp4
make rv-terrain
make all-rv
```

### Benchmarks
| Target | What it measures |
|--------|-----------------|
| `make benchmark-fragment` | GPU vs CPU across all 12 fragment shaders |
| `make benchmark-fragment-quick` | Same, fewer frames |
| `make benchmark-vertex` | Terrain vertex shader: GPU vs CPU |
| `make benchmark-compute` | Game of Life: GPU dispatch overhead at small grids |
| `make benchmark-compute-sweep` | Life crossover: grid sizes 16→512 |
| `make benchmark-compute-animate` | Life: GPU + CPU MP4 output |
| `make benchmark-compute-blur` | Gaussian blur: GPU compute vs CPU throughput |
| `make benchmark-diverge` | Branch divergence + warp boundary effect |
| `make benchmark-diverge-quick` | Quick divergence demo |
| `make cpu-scaling` | OpenMP thread scaling + Amdahl fit + RVV instruction count |
| `make cpu-scaling-quick` | Faster version |
| `make bench-rvv-width` | RVV vector width only (VLEN=128/256/512 under QEMU) |

---

## Language specification

### Types
- **Scalars**: `float`, `double`, `int`, `uint`, `bool`
- **Vectors**: `vec2`, `vec3`, `vec4`
- **Matrices**: `mat2x2` through `mat4x4`
- **Structs**: user-defined `struct`
- **Arrays**: local and uniform

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, unary `-` — on scalars and vectors
- Relational: `<`, `<=`, `>`, `>=`, `==`, `!=` — via `fcmp` / `icmp`
- Logical: `&&`, `||`, `!` — with short-circuit evaluation via conditional branches and phi nodes

### Control flow
`if` / `else`, `while`, `for`, `break`, `return`. Every basic block has an explicit terminator; `void` functions without an explicit `return` get `ret void` automatically.

### Built-in functions
`sin`, `cos`, `sqrt`, `floor`, `fract`, `dot`, `length`, `normalize`, `mix`, `clamp`, `min`, `max`, `mod`

### Swizzle
```glsl
vec3 v = c.xyz;       // read: shufflevector
vec2 p = v.xy;
vec3 rev = v.zyx;

v.xy = vec2(1.0, 2.0);  // write: insertelement chain
v.zyx = vec3(3.0, 2.0, 1.0);
```

### Uniforms
Declared with the `uniform` qualifier, emitted as LLVM `GlobalVariable` with `ExternalLinkage`:
```glsl
uniform float uTime;
uniform vec3  lightPos;
uniform mat4x4 MVP;
```

`vec3` uniforms require 16-byte alignment — host structs must add a `_pad` field:
```cpp
struct Vec3Uniform { float x, y, z, _pad; };
```

---

## Compilation pipeline details

### RISC-V path

```bash
# 1. Compile shader to LLVM IR
./build/riscv/irgen_riscv < shader_fs.src          # → module.ll

# 2. Link vertex + fragment modules
llvm-link-18 vs.ll fs.ll -S -o combined.ll

# 3. Optimise
opt-18 -O3 --enable-unsafe-fp-math --fp-contract=fast -S combined.ll -o opt.ll

# 4. sincos pass
opt-18 --load-pass-plugin=build/llvm/sincos_opt.so \
       -passes='sincos-opt,mem2reg,instcombine' -S opt.ll -o final.ll

# 5. Compile to RISC-V object
llc-18 -O3 --fp-contract=fast -filetype=obj \
       -mtriple=riscv64-unknown-linux-gnu -mattr=+m,+a,+f,+d,+v final.ll -o shader.o

# 6. Link with host + rasterizer
riscv64-linux-gnu-g++ -O3 -static -fopenmp -Ipipeline \
    -DANIM_NAME='"mandelbrot"' -DNFRAMES=60 \
    test/rv_host/rv_host_fragment.cpp pipeline/pipeline_runtime.cpp \
    shader.o -o mandelbrot.rv

# 7. Run under QEMU
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu ./mandelbrot.rv
```

### SPIR-V path

```bash
# 1. Compile shader to GLSL 450
./build/spirv/irgen_spirv < shader_fs.src           # → module.glsl

# 2. Compile to SPIR-V
glslangValidator -V --target-env vulkan1.0 module.glsl -o shader.frag.spv

# 3. Run via Vulkan host
./build/spirv/spirv_vulkan_host \
    build/spirv/quad.vert.spv shader.frag.spv mandelbrot 60 512 512
```

---

## Error handling

The compiler reports errors via `logError` and aborts code generation. Caught errors include:
- Syntax errors, type mismatches
- Invalid function calls, wrong argument counts
- Invalid swizzle or assignment targets
- Unknown struct fields

---

For benchmark details and test categories see [TESTING.md](TESTING.md).
For source layout and design decisions see [ARCHITECTURE.md](ARCHITECTURE.md).
