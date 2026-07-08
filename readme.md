# GLSL Compiler — RISC-V + Vulkan SPIR-V

A research compiler for a GLSL-inspired language that compiles shaders to two targets:
- **RISC-V LLVM IR** — runs on CPU via QEMU, parallelised with OpenMP, with optional RVV (RISC-V Vector Extension) auto-vectorisation
- **Vulkan SPIR-V** — runs on GPU via the Vulkan API (LavaPipe software renderer or real GPU)

The primary goal is to compare GPU and CPU execution of the same shader logic, measure GPU dispatch overhead, observe OpenMP thread scaling, and demonstrate RVV vectorisation.

---

## Project structure

```
.
├── src/
│   ├── frontend/       Language front-end
│   │   ├── lexer/        Tokeniser
│   │   ├── parser/       Recursive-descent parser → AST
│   │   ├── sema/         Semantic analysis
│   │   └── ast/          AST node definitions + codegen
│   ├── codegen/        IR generation back-end
│   │   ├── codegen_state/  Codegen context and symbol table
│   │   ├── helpers/        call_helpers, assignment_helpers, utils
│   │   └── emit/           irgen_riscv / irgen_spirv entry points + SPIR-V/trampoline emitters
│   ├── passes/         sincos_opt.cpp — LLVM pass plugin
│   ├── runtime/        Software rasterizer (pipeline_runtime.cpp/h, pipeline_abi.h, tex_inline)
│   ├── driver/         shader_codegen (interactive IR dump tool)
│   └── common/         error_utils helpers
├── test/
│   ├── assets/         3D mesh data (cube, Stanford bunny, textured jeep, high-poly teddy, Mixamo boss)
│   ├── rv_host/        RISC-V benchmark hosts (cross-compiled, run under QEMU)
│   │   ├── rv_host_fragment.cpp      generic animation host
│   │   ├── rv_host_compute.cpp       Game of Life CPU host
│   │   ├── rv_host_compute_blur.cpp  Gaussian blur CPU host
│   │   └── rv_host_mesh.cpp          indexed mesh CPU host
│   ├── vk_host/        Vulkan host programs (run on the host, drive the GPU)
│   │   ├── vk_host_fragment.cpp      offscreen animation renderer
│   │   ├── vk_host_compute.cpp       Game of Life Vulkan host
│   │   ├── vk_host_compute_blur.cpp  Gaussian blur Vulkan host
│   │   ├── vk_host_texture.cpp       texture sampling Vulkan host
│   │   ├── vk_host_mesh.cpp          indexed mesh Vulkan host
│   │   ├── mesh_data.h               Vertex/Mesh structs (shared with rv_host)
│   │   ├── icosphere.h               procedural icosphere generator
│   │   └── obj_loader.h              Wavefront OBJ parser
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

The project uses CMake. A one-time configure populates `build/`, after which any incremental rebuild is `cmake --build build`:

```bash
# First-time configure (Release by default)
cmake -S . -B build

# Build everything: build/{shader_codegen, riscv/irgen_riscv, spirv/irgen_spirv,
#                          spirv/spirv_vulkan_*, llvm/sincos_opt.so}
cmake --build build -j$(nproc)

# Or a single CMake target — names match the file basenames
cmake --build build --target irgen_spirv
cmake --build build --target mandelbrot.frag.spv
cmake --build build --target mandelbrot.rv
cmake --build build --target all-vk           # compile every Vulkan animation
cmake --build build --target benchmark-fragment

# Clean
rm -rf build result
```

Configuration discovers LLVM 18 (or 17), libfmt, Vulkan, spirv-headers, the RISC-V cross compiler (`riscv64-linux-gnu-g++`), and `qemu-riscv64-static`. Each is auto-detected — only LLVM, fmt, Vulkan, and spirv-headers are strictly required.

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
  └─ irgen_spirv → LLVM IR → emit_spirv_from_ir → .spv (binary, hand-rolled emitter)
                                                    │
                              vk_host_fragment + Vulkan API (LavaPipe) → frames → MP4
```

The sincos pass fuses `sin(x) + cos(x)` pairs into a single `sincosf(x, &s, &c)` call after `opt -O3` has unified duplicate arguments via GVN.

---

## CMake targets

All targets are invoked via `cmake --build build --target <name>`. The naming pattern:

- **`vk-<shader>` / `rv-<shader>`** — render one animation on the GPU (Vulkan) or
  CPU (RISC-V via QEMU) → `result/<shader>.mp4`. `all-vk` / `all-rv` build every one.
- **`vk-mesh*` / `rv-mesh*`** — the indexed-mesh demo (procedural icosphere or a
  Wavefront OBJ: bunny, jeep, teddy, boss). Same `mesh_vs.src` + `mesh_fs.src` for
  both backends; textured OBJs draw one `usemtl` range at a time with the matching
  `map_Kd` bound, sampled through the `llvm-link`'d bilinear sampler in
  `tex_inline.cpp`. The RISC-V rasterizer is two-pass tile-based, so it scales to
  the 1.5M-tri teddy without a per-triangle barrier. Renders 768×768.
- **`benchmark-*` / `cpu-scaling`** — GPU-vs-CPU benchmarks (fragment, vertex,
  mesh, compute, compute-blur, diverge) and the OpenMP/RVV scaling analysis.
- **`check`** — compiler unit tests (compile all test shaders, validate IR with `llvm-as`).

See **[TESTING.md](TESTING.md)** for the full target list, benchmark options, and what each measures.

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
riscv64-linux-gnu-g++ -O3 -static -fopenmp -Isrc/runtime \
    -DANIM_NAME='"mandelbrot"' -DNFRAMES=60 \
    test/rv_host/rv_host_fragment.cpp src/runtime/pipeline_runtime.cpp \
    shader.o -o mandelbrot.rv

# 7. Run under QEMU
OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu ./mandelbrot.rv
```

### SPIR-V path

The SPIR-V path is a single C++ translation unit ([emit_spirv_from_ir.h](src/codegen/emit/emit_spirv_from_ir.h)) that walks the LLVM IR and writes SPIR-V opcodes directly using the Khronos `spirv-headers`. No glslang, no `llvm-spirv`, no LLVM SPIR-V backend target — just a hand-rolled module-walker (~750 lines) that maps:

- `fadd/fsub/fmul/fdiv` → `OpFAdd / OpFSub / OpFMul / OpFDiv`
- `fcmp/icmp/select` → `OpFOrd* / OpS* / OpSelect`
- `extractelement / insertelement` → `OpComposite{Extract,Insert}`
- `alloca` → `OpVariable` in `Function` storage class (hoisted to entry block)
- `load / store` → `OpLoad / OpStore`
- LLVM intrinsics (`llvm.sin`, `llvm.cos`, `llvm.sqrt`, …) → `OpExtInst GLSL.std.450 Sin/Cos/Sqrt/…`
- `br i1` → `OpBranchConditional` preceded by `OpSelectionMerge` or `OpLoopMerge` (recovered from block-name patterns: `for.cond`, `then`/`ifend`, `logical.rhs`/`logical.merge`)
- External float globals → packed into a `Block`-decorated struct in `PushConstant` storage; loads rewritten to `OpAccessChain + OpLoad`
- Function args (`gl_FragCoord`, `vUV`, …) → `Input` `OpVariable`s with `BuiltIn` / `Location` decorations
- The `_out` trampoline pointer chain that `ast.cpp` codegens for the RISC-V ABI is detected and elided

```bash
# 1. Compile shader → .spv (no glslang involved)
./build/spirv/irgen_spirv shader.frag.spv < shader_fs.src

# 2. Validate (optional)
spirv-val shader.frag.spv

# 3. Run via Vulkan host
./build/spirv/spirv_vulkan_host \
    build/spirv/quad.vert.spv shader.frag.spv mandelbrot 60 512 512
```

---

## Error handling

All errors are routed through the helpers in [error_utils.h](src/common/error_utils.h) (basic logger, no fmt dependency — safe for cross-compiled riscv64 sources) and [error_utils_fmt.h](src/common/error_utils_fmt.h) (adds `logErrorFmt` / `logErrorContext`, requires `fmt::fmt` to be linked). Every error message is written to `stderr` with a `[ERROR]` prefix; `stdout` carries normal program output and benchmark results.

Caught categories include:
- Syntax errors, type mismatches
- Invalid function calls, wrong argument counts
- Invalid swizzle or assignment targets
- Unknown struct fields
- Vulkan API failures (`Vulkan error <code> at <call>`)
- File / shader-load I/O failures

---

For benchmark details and test categories see [TESTING.md](TESTING.md).
For source layout and design decisions see [ARCHITECTURE.md](ARCHITECTURE.md).
