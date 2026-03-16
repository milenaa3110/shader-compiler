# Project Architecture

This project is a GLSL-inspired shader compiler with two execution backends:
**RISC-V** (software rasterizer, runs under QEMU) and **Vulkan/SPIR-V** (GPU via LavaPipe).
A shader written in the custom `.src` language compiles all the way to a rendered MP4.

---

## How a shader becomes a video

```
shader_fs.src
   │
   ├─ build/riscv/irgen_riscv ──► vs.ll + fs.ll ──► llvm-link ──► opt -O3 ──► build/llvm/sincos_opt.so ──► llc ──► .o
   │                                                                                                     │
   │                                                               rv_host_fragment.cpp + pipeline_runtime.cpp
   │                                                                                                     │
   │                                                                                     riscv64 ELF (.rv)
   │                                                                                                     │
   │                                                                         QEMU ──► frames ──► MP4
   │
   └─ build/spirv/irgen_spirv ──► GLSL 450 ──► glslangValidator ──► .spv
                                                                │
                                            build/spirv/spirv_vulkan_host + Vulkan API (LavaPipe)
                                                                │
                                                         frames ──► MP4
```

---

## Directory map

### `lexer/`
| File | Role |
|------|------|
| `lexer.h` | Token enum — every keyword, operator, and type the language understands |
| `lexer.cpp` | Converts raw source text into a flat token stream |

### `parser/`
| File | Role |
|------|------|
| `parser.h` | Interface for the recursive-descent parser |
| `parser.cpp` | Builds an AST from the token stream; handles operator precedence, function definitions, and all statement forms |

### `ast/`
| File | Role |
|------|------|
| `ast.h` | All AST node types: literals, binary ops, variables, if/while/for, function defs, stage annotations (`@entry @stage`) |
| `ast.cpp` | `codegen()` on each node — walks the tree and emits LLVM IR via IRBuilder |

### `codegen_state/`
| File | Role |
|------|------|
| `codegen_state.h` | Singleton holding the LLVM Context, Module, IRBuilder, symbol table, uniform list, and break/continue stacks |
| `codegen_state.cpp` | Initializes and resets codegen state; provides helpers for vector splat, builtin lowering, and type queries |

### `helpers/`
| File | Role |
|------|------|
| `utils.h` | Type casting utilities, scalar↔vector conversions, swizzle character-to-index mapping |
| `call_helpers.h/.cpp` | Codegen for function calls: builtins (`sin`, `cos`, `dot`, `clamp`, `step`, …) and user-defined functions |
| `assignment_helpers.h/.cpp` | Codegen for assignments, including swizzle writes (`v.xyz = …`) via `insertelement` chains |

### `main/`
| File | Role |
|------|------|
| `main_lib_riscv.cpp` | Entry point for `irgen_riscv` — emits LLVM IR with `riscv64` target triple and RVV feature flags; emits VS+FS trampolines for the pipeline ABI |
| `main_lib_spirv.cpp` | Entry point for `irgen_spirv` — walks the AST and emits GLSL 450 source, then shells out to `glslangValidator` to produce `.spv` |
| `emit_trampolines.h` | Emits `vs_invoke` / `fs_invoke` wrapper functions and layout constants (`vs_total_floats`, `vs_varying_floats`) that the pipeline runtime calls |

### `main_codegen/`
| File | Role |
|------|------|
| `main_codegen.cpp` | Entry point for the interactive `shader_codegen` tool — reads stdin, prints IR to stdout |

### `pipeline/`
| File | Role |
|------|------|
| `pipeline_abi.h` | Shared structs: `PipelineDesc` (width, height, vert_count) and texture binding descriptors |
| `pipeline_runtime.h` | Declaration of `render_pipeline()` and the texture API used by fragment shaders |
| `pipeline_runtime.cpp` | Software rasterizer: vertex shading → triangle setup → rasterization → perspective interpolation → fragment shading → RGB framebuffer |
| `pipeline_host.cpp` | One-shot RISC-V pipeline host: links VS + FS objects, runs the pipeline, writes a PPM image |
| `anim_host.cpp` | Multi-frame RISC-V animation host: renders N frames and pipes raw RGB to ffmpeg |

### `passes/`
| File | Role |
|------|------|
| `sincos_opt.cpp` | LLVM pass plugin loaded by `opt-18 --load-pass-plugin`. Scans functions for `llvm.sin.f32(X)` + `llvm.cos.f32(X)` pairs and replaces them with a single `sincosf(X, &s, &c)` call. Runs after `opt -O3` (which unifies identical arguments via GVN), so pairs are reliably detected. Saves one trig range-reduction per matched pair at runtime. |

### `test/shaders/`

| Subdirectory | Contents |
|--------------|----------|
| `test/shaders/*.src` | Unit-test shaders — one feature per file (arrays, builtins, matrix assign, swizzle, texture, uniforms, …) |
| `test/shaders/pipeline/` | VS + FS pairs used by the software pipeline tests (`triangle`, `scene`, `anim`) |
| `test/shaders/animations/` | All production animation shaders — see table below |

**Animation shaders** (`test/shaders/animations/`):

| Shader | Type | Description |
|--------|------|-------------|
| `mandelbrot_fs.src` | FS | Mandelbrot set fractal |
| `julia_fs.src` | FS | Julia set fractal |
| `voronoi_fs.src` | FS | Voronoi diagram |
| `waves_fs.src` | FS | Sinusoidal wave field |
| `tunnel_fs.src` | FS | Infinite tunnel |
| `ripple_fs.src` | FS | Ripple/interference pattern |
| `galaxy_fs.src` | FS | Spiral galaxy |
| `fire_fs.src` | FS | Fire effect |
| `reaction_fs.src` | FS | Reaction-diffusion |
| `cellular_fs.src` | FS | Cellular automata |
| `earth_fs.src` | FS | Ray-sphere earth with day/night |
| `scene3d_fs.src` | FS | 3D raymarched scene |
| `diverge_fs.src` | FS | Branch-divergence stress test |
| `texture_test_fs.src` | FS | Texture sampling (RISC-V) |
| `texture_test_gpu_fs.src` | FS | Texture sampling (Vulkan) |
| `terrain_vs.src` | VS | 32×32 animated terrain mesh — RISC-V build (standard Y) |
| `terrain_vs_vk.src` | VS | Same terrain mesh — Vulkan build (Y negated for Vulkan NDC, Vulkan z in [0,1]) |
| `terrain_fs.src` | FS | Pass-through fragment shader for terrain (outputs `vColor`) |
| `quad_vs.src` | VS | Screen-covering quad used as the VS for all fragment-only animations |
| `life.comp` | CS | Game of Life compute shader (GLSL, compiled with glslangValidator) |
| `blur.comp` | CS | Blur compute shader (GLSL, compiled with glslangValidator) |

### `test/rv_host/`
RISC-V host programs — cross-compiled to `riscv64` and run under QEMU.

| File | Role |
|------|------|
| `rv_host_fragment.cpp` | Generic animation host — compiled with `-DANIM_NAME`, `-DVERT_COUNT`, etc. Streams raw RGB frames to ffmpeg; reports ms/frame. Mirrors `vk_host_fragment.cpp`. |
| `rv_host_compute_blur.cpp` | CPU Gaussian blur host (mirrors `vk_host_compute_blur.cpp`) |
| `rv_host_compute.cpp` | CPU Game of Life host (mirrors `vk_host_compute.cpp`) |

### `test/vk_host/`
Vulkan host programs — run on the host CPU, drive the GPU via the Vulkan API.

| File | Role |
|------|------|
| `vk_host_fragment.cpp` | Offscreen renderer: loads a VS + FS `.spv`, streams frames to ffmpeg |
| `vk_host_compute_blur.cpp` | Compute host: dispatches `blur.comp.spv` on storage buffers |
| `vk_host_compute.cpp` | Game of Life host: ping-pong compute dispatch + readback |
| `vk_host_texture.cpp` | Texture host: combined image sampler, animated UV distortion |

### `test/script/` (scripts)
| File | Role |
|------|------|
| `bench_common.sh` | Shared library: color codes, QEMU detection, `parse_avg`, `speedup_label` |
| `run_tests.sh` | Unit test runner — compiles each test shader with `irgen_riscv` and validates the LLVM IR |
| `run_benchmark_fragment.sh` | Main benchmark: all 13 fragment animations, VK vs RV, prints comparison table |
| `run_benchmark_vertex.sh` | Terrain (vertex shader) benchmark: VS/FS/RV sizes + ms/frame table |
| `run_benchmark_compute_blur.sh` | Compute (blur) benchmark |
| `run_benchmark_compute.sh` | Game of Life benchmark (tiny / sweep / animate modes) |
| `run_benchmark_diverge.sh` | Branch-divergence benchmark across resolutions |
| `run_cpu_scaling.sh` | OpenMP thread scaling + Amdahl fit + RVV instruction-count demo |

### `build/` (generated)
| Path | Contents |
|------|----------|
| `build/shader_codegen` | Interactive IR dump tool |
| `build/riscv/irgen_riscv` | Compiler binary — riscv64 IR + trampolines |
| `build/riscv/` | RISC-V intermediates (`.ll`, `.o`) and `.rv` binaries |
| `build/spirv/irgen_spirv` | Compiler binary — GLSL 450 emitter |
| `build/spirv/spirv_vulkan_host` | Vulkan animation host |
| `build/spirv/spirv_vulkan_compute_host` | Vulkan compute host |
| `build/spirv/spirv_vulkan_life_host` | Vulkan Game of Life host |
| `build/spirv/spirv_vulkan_texture_host` | Vulkan texture host |
| `build/spirv/` | Compiled SPIR-V bytecode (`.spv`) |
| `build/llvm/sincos_opt.so` | LLVM pass plugin |
| `build/llvm/` | Compiler object files |

---

## Optimization pipeline (RISC-V shaders)

Every `.src` shader goes through five stages before becoming a `.o` object:

```
build/riscv/irgen_riscv < shader.src      # parse → AST → raw LLVM IR  (.ll)
llvm-link-18 vs.ll fs.ll            # merge vertex + fragment modules
opt-18 -O3                          # GVN, CSE, instcombine, SROA, fast-math, FMA
opt-18 --load-pass-plugin=build/llvm/sincos_opt.so
       -passes='sincos-opt,mem2reg,instcombine'
                                    # combine sin(X)+cos(X) → sincosf(X,&s,&c)
llc-18 -O3 --fp-contract=fast       # instruction selection → riscv64 machine code (.o)
```

---

## Key design decisions

- **No vertex buffers** — the terrain and all procedural geometry is computed entirely from `gl_VertexID`. No data upload, no binding.
- **Push constants for uniforms** — `uniform float uTime` is emitted as a Vulkan push constant and a RISC-V global; no descriptor sets needed.
- **Direct ffmpeg pipe** — both `spirv_vulkan_host` and `rv_host_fragment` open an ffmpeg pipe before the frame loop and stream raw RGB. No PPM files are written.
- **Two terrain vertex shaders** — RISC-V rasterizer flips Y internally (`sy = (-ny+1)*0.5*H`); Vulkan NDC has Y+ at bottom. So `terrain_vs.src` uses standard projection and `terrain_vs_vk.src` negates Y and uses the Vulkan z formula (`A = far/(far-near)`, result in [0,1]).
- **All build artifacts in `build/`** — compiler binaries, plugins, SPIR-V bytecode, and RISC-V intermediates all land under `build/`. Only source files and test shaders live in the repo root and `test/`.
- **SPV files in `build/spirv/`** — all compiled SPIR-V bytecode lives in `build/spirv/`; `result/` contains only MP4 output.
