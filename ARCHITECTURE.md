# Project Architecture

This project is a GLSL-inspired shader compiler with two execution backends:
**RISC-V** (software rasterizer, runs under QEMU) and **Vulkan/SPIR-V** (GPU via LavaPipe).
A shader written in the custom `.src` language compiles all the way to a rendered MP4.

---

## How a shader becomes a video

```
shader.src
   │
   ├─ build/riscv/irgen_riscv ──► vs.ll + fs.ll ──► llvm-link ──► opt -O3
   │                                                         + sincos-opt pass plugin
   │                                                                    │
   │                                                                  llc-18
   │                                                                    │
   │                                                          riscv64 .o (RV64GCV)
   │                                                                    │
   │                              rv_host_*.cpp + pipeline_runtime.cpp ──┘
   │                                                                    │
   │                                                          riscv64 ELF (.rv)
   │                                                                    │
   │                                                  QEMU ──► frames ──► MP4
   │
   └─ build/spirv/irgen_spirv ──► LLVM IR ──► emit_spirv_from_ir ──► .spv
                                                                       │
                                            build/spirv/spirv_vulkan_*_host
                                            + Vulkan API (LavaPipe / iGPU / dGPU)
                                                                       │
                                                              frames ──► MP4
```

The SPIR-V backend is hand-rolled: there is no `glslangValidator`, no `llvm-spirv`,
no LLVM SPIR-V target. `emit_spirv_from_ir.h` walks the LLVM IR module and emits
SPIR-V bytecode word-by-word.

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
| `main_lib_riscv.cpp` | Entry point for `irgen_riscv` — emits LLVM IR with `riscv64` target triple and RVV feature flags; emits VS+FS+CS trampolines for the pipeline ABI |
| `main_lib_spirv.cpp` | Entry point for `irgen_spirv` — runs the same AST → LLVM IR codegen as `irgen_riscv`, then emits SPIR-V binary directly via `emit_spirv_from_ir.h` |
| `emit_spirv_from_ir.h` | LLVM IR → SPIR-V binary translator. Handles types/constants, structured control flow, push constants for `uniform float`, runtime-sized SSBOs (`Uniform` storage class with `BufferBlock` decoration) for compute shaders, vertex input attributes, fragment output `Location 0`, GLCompute `LocalSize` from `!shader.workgroup_size` metadata |
| `emit_trampolines.h` | Emits `vs_invoke(vid, iid, flat_in, flat_out)` / `fs_invoke(fragcoord, varyings, flat_out)` trampolines plus the layout constants `vs_total_floats`, `vs_varying_floats`, `vs_input_floats`, `fs_output_floats`. Also emits `cs_invoke` (single invocation) and `cs_dispatch_row` (X-loop, allows RVV auto-vectorization) for compute shaders |

### `main_codegen/`
| File | Role |
|------|------|
| `main_codegen.cpp` | Entry point for the interactive `shader_codegen` tool — reads stdin, prints IR to stdout |

### `pipeline/`
| File | Role |
|------|------|
| `pipeline_abi.h` | Stable C ABI between the software rasterizer and a compiled VS+FS pipeline module: `vs_invoke` / `fs_invoke` declarations and the `vs_total_floats` / `vs_varying_floats` / `vs_input_floats` / `fs_output_floats` layout constants |
| `pipeline_runtime.h` | `PipelineDesc { width, height, vert_count, vbuf, indices, index_count, first_index, clear }` and the `bind_texture` API used by software texture sampling |
| `pipeline_runtime.cpp` | Software rasterizer — two-pass **tile-based** design. Pass 1 (parallel over triangles): runs the vertex shader for every vertex, then for each triangle computes its screen-space setup, culls back-facing / off-screen / degenerate cases, and bins survivors into the 32×32 screen tiles their bbox overlaps. Pass 2 (parallel over tiles): each tile is owned by one thread and rasterises its bin with perspective-correct interpolation — no per-triangle barrier and no z-buffer race, so it scales to multi-million-triangle meshes |
| `tex_inline.cpp/.h` | Bilinear texture sampler (`__tex_lookup`, `__tex2d_sample`). Compiled to LLVM bitcode and `llvm-link`'d into each shader module before `opt -O3`, so the `always_inline` sampler bodies expand directly into the fragment shader's hot loop. `g_tex` storage and the host-facing `bind_texture` live in `pipeline_runtime.cpp` |

### `passes/`
| File | Role |
|------|------|
| `sincos_opt.cpp` | LLVM pass plugin loaded by `opt-18 --load-pass-plugin`. Scans functions for `llvm.sin.f32(X)` + `llvm.cos.f32(X)` pairs and replaces them with a single `sincosf(X, &s, &c)` call. Runs after `opt -O3` (which unifies identical arguments via GVN), so pairs are reliably detected |

### Project-root error helpers
| File | Role |
|------|------|
| `error_utils.h` | Minimal logger: `logError(const char*)` / `logError(const std::string&)`. No fmt dependency — safe to include from cross-compiled (riscv64) sources |
| `error_utils_fmt.h` | Adds `logErrorFmt("...{}...", arg)` and `logErrorContext(ctx, msg)` on top of `error_utils.h`. Requires `fmt::fmt` to be linked (host-only) |

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
| `terrain_vs.src` | VS | 32×32 animated terrain mesh — RISC-V build (no Y flip) |
| `terrain_vs_vk.src` | VS | Terrain — Vulkan build (Y negated for Vulkan NDC, Vulkan z in [0,1]) |
| `terrain_fs.src` | FS | Pass-through fragment shader for terrain (outputs `vColor`) |
| `quad_vs.src` | VS | Screen-covering quad used as the VS for all fragment-only animations |
| `mesh_vs.src` | VS | Indexed-mesh VS — reads `aPos`/`aNormal`/`aUV` from a VBO, applies a Y-axis orbit + perspective projection. Same source compiles for GPU and CPU; the Vulkan host uses a negative-height viewport to handle Vulkan's NDC convention |
| `mesh_fs.src` | FS | Textured mesh fragment shader — samples a `map_Kd` albedo and combines it with Lambert + Blinn-Phong + fresnel rim lighting, modulated by the per-material `uKd` |
| `life_cs.src` | CS | Game of Life compute shader — compiled via `irgen_spirv` (SPIR-V) and `irgen_riscv` (RISC-V) |
| `blur_cs.src` | CS | 5-tap horizontal Gaussian blur — compiled via `irgen_spirv` and `irgen_riscv` |

### `test/assets/`
3D mesh data consumed by the indexed-mesh demo:
- `cube.obj` — minimal sanity-check mesh (8 unique positions / 12 tris)
- `bunny.obj` — Stanford bunny (2503 verts / 4968 tris, no MTL)
- `Jeep_Renegade_2016.obj` + `.mtl` + `car_jeep_ren.jpg` — textured vehicle (4728 tris, multi-material)
- `teddy-bear/` — high-poly textured teddy (1.5M tris, PBR texture set) — stresses the tile-based rasterizer
- `boss/` — textured Mixamo "boss" character (10220 tris / 30660 verts, `Rumba Dancing.obj` + `.mtl` + `Clothes_MAT.png`)

### `test/rv_host/`
RISC-V host programs — cross-compiled to `riscv64` and run under QEMU.

| File | Role |
|------|------|
| `rv_host_fragment.cpp` | Generic animation host — compiled with `-DANIM_NAME`, `-DVERT_COUNT`, etc. Streams raw RGB frames to ffmpeg; reports ms/frame. Mirrors `vk_host_fragment.cpp` |
| `rv_host_compute_blur.cpp` | CPU Gaussian blur host (mirrors `vk_host_compute_blur.cpp`) |
| `rv_host_compute.cpp` | CPU Game of Life host (mirrors `vk_host_compute.cpp`) |
| `rv_host_mesh.cpp` | CPU indexed-mesh host. Loads an icosphere or OBJ via the shared `vk_host` headers, flattens vertices into a contiguous float buffer, and drives `render_pipeline` with the VBO+IBO. Mirrors `vk_host_mesh.cpp` |

### `test/vk_host/`
Vulkan host programs — run on the host CPU, drive the GPU via the Vulkan API.

| File | Role |
|------|------|
| `vk_host_fragment.cpp` | Offscreen renderer: loads a VS + FS `.spv`, streams frames to ffmpeg |
| `vk_host_compute_blur.cpp` | Compute host: dispatches `blur.comp.spv` on storage buffers |
| `vk_host_compute.cpp` | Game of Life host: ping-pong compute dispatch + readback |
| `vk_host_texture.cpp` | Texture host: combined image sampler, animated UV distortion |
| `vk_host_mesh.cpp` | Indexed-mesh host: per-material VBO + IBO upload via staging buffer, depth attachment, negative-height viewport for Vulkan Y-flip, per-range textured draws, optional MP4 + PPM output |
| `mesh_data.h` | `Vertex { pos[3], normal[3], uv[2] }`, `Material`, `MaterialRange`, and `Mesh { vertices, indices, materials, ranges }` — shared between Vulkan and RISC-V mesh hosts |
| `icosphere.h` | Procedural icosphere generator: subdivision-controlled triangle count (20 → 81920 tris). Used to vary load on demand without external assets |
| `obj_loader.h` | Wavefront OBJ + MTL parser. Handles `v`/`vn`/`vt`/`f` (`pos/uv/normal` triples, `pos//normal` pairs), `mtllib`/`usemtl` with per-material draw ranges, and `map_Kd` diffuse-texture paths resolved relative to the OBJ dir. Computes face-averaged normals when the file lacks `vn`; `mtllib` reads the full line so filenames with spaces parse correctly |

### `test/script/`
| File | Role |
|------|------|
| `bench_common.sh` | Shared library: color codes, QEMU detection, `parse_avg`, `speedup_label` |
| `run_tests.sh` | Unit test runner — compiles each test shader with `irgen_riscv` and validates the LLVM IR |
| `run_benchmark_fragment.sh` | Main benchmark: all 13 fragment animations, VK vs RV, prints comparison table |
| `run_benchmark_vertex.sh` | Terrain (vertex shader) benchmark: VS/FS/RV sizes + ms/frame table |
| `run_benchmark_mesh.sh` | Indexed-mesh benchmark: the textured "boss" OBJ through the full VBO+IBO pipeline, VK vs RV, sizes + ms/frame table |
| `run_benchmark_compute_blur.sh` | Compute (blur) benchmark |
| `run_benchmark_compute.sh` | Game of Life benchmark (tiny / sweep / animate modes) |
| `run_benchmark_diverge.sh` | Branch-divergence benchmark across resolutions |
| `run_cpu_scaling.sh` | OpenMP thread scaling + Amdahl fit + RVV instruction-count demo |

### `build/` (generated)
| Path | Contents |
|------|----------|
| `build/shader_codegen` | Interactive IR dump tool |
| `build/riscv/irgen_riscv` | Compiler binary — riscv64 IR + trampolines |
| `build/riscv/` | RISC-V intermediates (`.ll`, `.gvn.ll`, `.opt.ll`, `.o`) and `.rv` binaries |
| `build/spirv/irgen_spirv` | Compiler binary — SPIR-V emitter |
| `build/spirv/spirv_vulkan_host` | Vulkan animation host |
| `build/spirv/spirv_vulkan_compute_host` | Vulkan compute (blur) host |
| `build/spirv/spirv_vulkan_life_host` | Vulkan Game of Life host |
| `build/spirv/spirv_vulkan_texture_host` | Vulkan texture host |
| `build/spirv/spirv_vulkan_mesh_host` | Vulkan indexed-mesh host |
| `build/spirv/` | Compiled SPIR-V bytecode (`.spv`) |
| `build/llvm/sincos_opt.so` | LLVM pass plugin |
| `build/llvm/` | Compiler object files |

---

## Pipeline ABI (RISC-V side)

The trampoline emitter writes a stable C ABI into every shader module so that
`pipeline_runtime.cpp` can drive the compiled shaders without knowing their
specific signatures:

```c
void vs_invoke(int vid, int iid, float* flat_in, float* flat_out);
//   flat_in  — vs_input_floats per-vertex attribute floats (NULL if zero)
//   flat_out — vs_total_floats: gl_Position(4) + varyings

void fs_invoke(float* fragcoord, float* varyings, float* flat_out);

extern int vs_total_floats;    // gl_Position(4) + all VS out-vars
extern int vs_varying_floats;  // VS out-vars only (interpolated)
extern int vs_input_floats;    // per-vertex attribute floats (0 = none)
extern int fs_output_floats;   // floats in FS_Output (e.g. 4 for vec4 FragColor)
```

`PipelineDesc` carries either an indexed mesh (`vbuf` + `indices`) or, when both
are NULL, the legacy `gl_VertexID`-synthesized geometry path (terrain, full-screen
quads).

---

## Optimization pipeline (RISC-V shaders)

Every `.src` shader goes through five stages before becoming a `.o` object:

```
build/riscv/irgen_riscv < shader.src         # parse → AST → raw LLVM IR  (.ll)
llvm-link-18 vs.ll fs.ll                     # merge vertex + fragment modules
opt-18 -O3                                   # GVN, CSE, instcombine, SROA, fast-math, FMA
opt-18 --load-pass-plugin=build/llvm/sincos_opt.so
       -passes='sincos-opt,mem2reg,instcombine'
                                             # combine sin(X)+cos(X) → sincosf(X,&s,&c)
llc-18 -O3 --fp-contract=fast                # instruction selection → riscv64 machine code (.o)
```

---

## Key design decisions

- **Hand-rolled SPIR-V emitter** — `emit_spirv_from_ir.h` translates LLVM IR
  directly to SPIR-V bytecode, with no glslang/llvm-spirv intermediate. Pinned
  to SPIR-V 1.0 for Vulkan 1.0 compatibility, which means SSBOs use the
  `Uniform` storage class with the `BufferBlock` decoration rather than the
  newer `StorageBuffer` storage class.
- **Two parallel rendering paths** — most fragment-only animations synthesize
  geometry from `gl_VertexID` (no VBOs, no descriptor sets); the indexed-mesh
  demo uses real vertex buffers + index buffers, exercising the full vertex
  input / per-vertex attribute path on both backends.
- **Same shader source for GPU and CPU** — the mesh demo compiles a single
  `mesh_vs.src` for both backends. Vulkan handles the flipped Y NDC via a
  negative-height viewport, so the shader's `gl_Position.y` is unchanged.
  Terrain still has a separate `terrain_vs_vk.src` because it pre-dates the
  negative-viewport idiom.
- **Push constants for scalar uniforms** — `uniform float uTime` becomes a
  Vulkan push constant on the GPU side and a plain global on the RISC-V side;
  no descriptor sets needed for simple uniforms.
- **Direct ffmpeg pipe** — the Vulkan and RISC-V hosts both open an ffmpeg
  pipe before the frame loop and stream raw RGB. No intermediate PPM frames are
  written (a single mid-animation PPM is saved for diffing).
- **All build artifacts in `build/`** — compiler binaries, plugins, SPIR-V
  bytecode, and RISC-V intermediates all land under `build/`. Only source files
  and test assets live in the repo root and `test/`.
- **Unified error logging** — every host and compiler tool emits errors via
  `logError(...)` (or `logErrorFmt("{}", arg)` where formatting is needed)
  from `error_utils.h` / `error_utils_fmt.h`. Errors are prefixed `[ERROR]`
  on `stderr`; stdout is reserved for normal program output.
