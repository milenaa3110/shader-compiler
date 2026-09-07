# Project Architecture

This project is a GLSL-inspired shader compiler with two execution backends:
**RISC-V** (software rasterizer, run under QEMU) and **Vulkan/SPIR-V** (GPU via LavaPipe).
A shader written in the custom `.src` language is compiled all the way to a rendered MP4.

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
no LLVM SPIR-V target. The LLVM IR module is walked by `emit_spirv_from_ir.h` and
SPIR-V bytecode is emitted word-by-word.

---

## Directory map

### `src/frontend/lexer/`
| File | Role |
|------|------|
| `lexer.h` | Token type + the `Token` struct (stamped with a `SourceLocation` byte offset); interface for the tokeniser |
| `lexer.cpp` | Raw source text is converted into a flat token stream |
| `tokens.def` | X-macro list of every keyword, operator, and punctuator — the single source of truth shared by the token enum and the lexer's keyword table |

### `src/frontend/parser/`
| File | Role |
|------|------|
| `parser.h` | Interface for the recursive-descent parser |
| `parser.cpp` | An AST is built from the token stream; operator precedence, function definitions, and all statement forms are handled. Pure syntax — type-level checks are deferred to sema. Nodes are allocated out of the `ASTContext` arena |

### `src/frontend/sema/`
| File | Role |
|------|------|
| `sema.h` | Interface for the post-parse semantic analysis pass |
| `sema.cpp` | The AST is walked and the invariants deferred by the parser are enforced: struct declarations are collected, cyclic field dependencies are detected, and every type-name string is validated to resolve to a builtin or a declared struct. Reporting is done through the same `logErrorAt` channel as the parser; the error count is returned by `run()` and codegen is refused by callers when it is nonzero |

### `src/frontend/ast/`
| File | Role |
|------|------|
| `ast.h` | All AST node types: literals, binary ops, variables, if/while/for, function defs, stage annotations (`@entry @stage`) |
| `ast.cpp` | `codegen()` on each node — the tree is walked and LLVM IR is emitted via IRBuilder |
| `ast_context.h/.cpp` | Per-compilation arena in which every AST node and interned string is owned. Placement-new into a `BumpPtrAllocator` is done by `create<T>(...)` (no per-node new/delete; the whole tree is freed in one shot); a stable, content-shared `StringRef` is returned by `intern(s)`. Also the factory in which `Type` instances are uniqued |
| `type.h` | The language's semantic type system, modelled on `llvm::Type`: exactly one instance of each distinct type is kept (uniqued by `ASTContext`), so type equality is a pointer compare. Scoped as `glsl::Type` so that collision with `llvm::Type` is avoided. Being brought up incrementally — nodes are stamped by sema, and it will be dispatched on by codegen instead of `float` being assumed |
| `builtin_types.def` | X-macro table of the builtin scalar/vector/matrix/sampler/image types — shared by the type system and type-name resolution |

### `src/codegen/codegen_state/`
| File | Role |
|------|------|
| `codegen_state.h` | Singleton holding the LLVM Context, Module, IRBuilder, symbol table, uniform list, and break/continue stacks |
| `codegen_state.cpp` | Codegen state is initialized and reset; helpers are provided for vector splat, builtin lowering, and type queries |

### `src/codegen/helpers/`
| File | Role |
|------|------|
| `utils.h` | Type casting utilities, scalar↔vector conversions, swizzle character-to-index mapping |
| `call_helpers.h/.cpp` | Codegen for function calls: builtins (`sin`, `cos`, `dot`, `clamp`, `step`, …) and user-defined functions |
| `assignment_helpers.h/.cpp` | Codegen for assignments, including swizzle writes (`v.xyz = …`) via `insertelement` chains |

### `src/codegen/emit/`
| File | Role |
|------|------|
| `main_lib_riscv.cpp` | Entry point for `irgen_riscv` — LLVM IR is emitted with `riscv64` target triple and RVV feature flags; VS+FS+CS trampolines are emitted for the pipeline ABI |
| `main_lib_spirv.cpp` | Entry point for `irgen_spirv` — the same AST → LLVM IR codegen as `irgen_riscv` is run, then SPIR-V binary is emitted directly via `emit_spirv_from_ir.h` |
| `emit_spirv_from_ir.h` | LLVM IR → SPIR-V binary translator. Types/constants, structured control flow, push constants for `uniform float`, runtime-sized SSBOs (`Uniform` storage class with `BufferBlock` decoration) for compute shaders, vertex input attributes, fragment output `Location 0`, and GLCompute `LocalSize` from `!shader.workgroup_size` metadata are handled |
| `emit_trampolines.h` | `vs_invoke(vid, iid, flat_in, flat_in_d, flat_out)` / `fs_invoke(fragcoord, varyings, flat_out, flat_out_d)` trampolines are emitted, plus the layout constants `vs_total_floats`, `vs_varying_floats`, `vs_input_floats`/`vs_input_doubles`, `fs_output_floats`/`fs_output_doubles` (64-bit `double` I/O is carried in the `_d` regions on the non-interpolated surfaces). `cs_invoke` and `cs_dispatch_row` are also emitted for compute shaders |
| `emit_fs_packet.h` | **SPMD packetizer (Route B).** A width-4 `fs_packet`/`vs_packet` is emitted, by which 4 fragments/vertices are run per call, one per SIMD lane (struct-of-arrays). One stage-agnostic engine is reused: arithmetic, swizzles, math builtins, `if`/`else`+loops+`break`/`continue` via execution masks, `discard` (liveness side-channel), texture gather. For shaders outside the subset the emission is abandoned (no `*_packet` is emitted) and the scalar path is used by the runtime. Selected at run time by `SHADER_PACKET=1` |

### `src/runtime/`
| File | Role |
|------|------|
| `pipeline_abi.h` | Stable C ABI between the software rasterizer and a compiled VS+FS pipeline module: `vs_invoke` / `fs_invoke` declarations and the `vs_total_floats` / `vs_varying_floats` / `vs_input_floats` / `fs_output_floats` layout constants |
| `pipeline_runtime.h` | `PipelineDesc { width, height, vert_count, vbuf, indices, index_count, vbuf_d, first_index, clear }` and the `bind_texture` API used by software texture sampling |
| `pipeline_runtime.cpp` | Software rasterizer — two-pass **tile-based** design. Pass 1 (parallel over triangles): the vertex shader is run for every vertex, then for each triangle its screen-space setup is computed, back-facing / off-screen / degenerate cases are culled, and survivors are binned into the 32×32 screen tiles their bbox overlaps. Pass 2 (parallel over tiles): each tile is owned by one thread and its bin is rasterised with perspective-correct interpolation — no per-triangle barrier and no z-buffer race, so multi-million-triangle meshes are handled. **FS/VS dispatch is batched**: when `SHADER_PACKET=1` and the shader packetized, accepted fragments / vertices are collected into width-4 SoA groups and run through `fs_packet`/`vs_packet` (weak symbols; scalar `fs_invoke`/`vs_invoke` fallback otherwise) — bit-identical to the scalar path |
| `tex_inline.cpp/.h` | Bilinear texture sampler (`__tex_lookup`, `__tex2d_sample`). Compiled to LLVM bitcode and `llvm-link`'d into each shader module before `opt -O3`, so the `always_inline` sampler bodies are expanded directly into the fragment shader's hot loop. `g_tex` storage and the host-facing `bind_texture` are kept in `pipeline_runtime.cpp` |

### `src/passes/`
| File | Role |
|------|------|
| `sincos_opt.cpp` | LLVM pass plugin loaded by `opt-18 --load-pass-plugin`. Functions are scanned for `llvm.sin.f32(X)` + `llvm.cos.f32(X)` pairs and they are replaced with a single `sincosf(X, &s, &c)` call. Run after `opt -O3` (by which identical arguments are unified via GVN), so pairs are reliably detected |

### `src/common/` (error helpers)
| File | Role |
|------|------|
| `error_utils.h` | Minimal logger: `logError(const char*)` / `logError(const std::string&)`. No fmt dependency — safe to include from cross-compiled (riscv64) sources |
| `error_utils_fmt.h` | `logErrorFmt("...{}...", arg)` and `logErrorContext(ctx, msg)` are added on top of `error_utils.h`. `fmt::fmt` must be linked (host-only) |
| `source_manager.h` | `SourceLocation` (an opaque byte offset, modelled on LLVM's `SMLoc`) and a `SourceManager` in which `(line, column)` and the source line are reconstructed lazily — only when a diagnostic is printed. `Token` is kept small and `advance()` branch-free; the `logErrorAt` diagnostics shared by parser and sema are backed by it |

### `test/shaders/`
| Subdirectory | Contents |
|--------------|----------|
| `test/shaders/compiler_tests/` | Front-end unit shaders — one feature per file (arrays, builtins, matrix assign, swizzle, ternary, struct, texture, uniforms, bitwise, operators, …); compiled to IR and validated by `run_tests.sh` |
| `test/shaders/codegen_checks/` | Codegen-correctness shaders in which specific lowering is pinned down (vector/argument coercion, matrix multiply, short-circuit, ternary dominance, shift signedness, bool widening, stage-builtin coercion) — checked by `run_ir_checks.sh` |
| `test/shaders/pipeline/` | VS + FS pairs used by the software pipeline tests (`triangle`, `scene`, `anim`) |
| `test/shaders/animations/` | All production animation shaders — given in the table below |

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
| `city_fs.src` | FS | Procedural city skyline |
| `ocean_fs.src` | FS | Animated ocean surface |
| `diverge_fs.src` | FS | Branch-divergence stress test |
| `texture_test_fs.src` | FS | Texture sampling (RISC-V) |
| `texture_test_gpu_fs.src` | FS | Texture sampling (Vulkan) |
| `terrain_vs.src` | VS | 32×32 animated terrain mesh — RISC-V build (no Y flip) |
| `terrain_vs_vk.src` | VS | Terrain — Vulkan build (Y negated for Vulkan NDC, Vulkan z in [0,1]) |
| `terrain_fs.src` | FS | Pass-through fragment shader for terrain (outputs `vColor`) |
| `quad_vs.src` | VS | Screen-covering quad used as the VS for all fragment-only animations |
| `mesh_vs.src` | VS | Indexed-mesh VS — `aPos`/`aNormal`/`aUV` are read from a VBO and a Y-axis orbit + perspective projection is applied. The same source is compiled for GPU and CPU; a negative-height viewport is used by the Vulkan host so that Vulkan's NDC convention is handled |
| `mesh_fs.src` | FS | Textured mesh fragment shader — a `map_Kd` albedo is sampled and combined with Lambert + Blinn-Phong + fresnel rim lighting, modulated by the per-material `uKd` |
| `life_cs.src` | CS | Game of Life compute shader — compiled via `irgen_spirv` (SPIR-V) and `irgen_riscv` (RISC-V) |
| `blur_cs.src` | CS | 5-tap horizontal Gaussian blur — compiled via `irgen_spirv` and `irgen_riscv` |

### `test/assets/`
3D mesh data consumed by the indexed-mesh demo:
- `cube.obj` — minimal sanity-check mesh (8 unique positions / 12 tris)
- `bunny.obj` — Stanford bunny (2503 verts / 4968 tris, no MTL)
- `Jeep_Renegade_2016.obj` + `.mtl` + `car_jeep_ren.jpg` — textured vehicle (4728 tris, multi-material)
- `teddy-bear/` — high-poly textured teddy (1.5M tris, PBR texture set) — by which the tile-based rasterizer is stressed
- `boss/` — textured Mixamo "boss" character (10220 tris / 30660 verts, `Rumba Dancing.obj` + `.mtl` + `Clothes_MAT.png`)

### `test/rv_host/`
RISC-V host programs — cross-compiled to `riscv64` and run under QEMU.

| File | Role |
|------|------|
| `rv_host_fragment.cpp` | Generic animation host — compiled with `-DANIM_NAME`, `-DVERT_COUNT`, etc. Raw RGB frames are streamed to ffmpeg; ms/frame is reported. Mirrored on `vk_host_fragment.cpp` |
| `rv_host_compute_blur.cpp` | CPU Gaussian blur host (mirrored on `vk_host_compute_blur.cpp`) |
| `rv_host_compute.cpp` | CPU Game of Life host (mirrored on `vk_host_compute.cpp`) |
| `rv_host_mesh.cpp` | CPU indexed-mesh host. An icosphere or OBJ is loaded via the shared `vk_host` headers, vertices are flattened into a contiguous float buffer, and `render_pipeline` is driven with the VBO+IBO. Mirrored on `vk_host_mesh.cpp` |

### `test/vk_host/`
Vulkan host programs — run on the host CPU; the GPU is driven via the Vulkan API.

| File | Role |
|------|------|
| `vk_host_fragment.cpp` | Offscreen renderer: a VS + FS `.spv` is loaded and frames are streamed to ffmpeg |
| `vk_host_compute_blur.cpp` | Compute host: `blur.comp.spv` is dispatched on storage buffers |
| `vk_host_compute.cpp` | Game of Life host: ping-pong compute dispatch + readback |
| `vk_host_texture.cpp` | Texture host: combined image sampler, animated UV distortion |
| `vk_host_mesh.cpp` | Indexed-mesh host: per-material VBO + IBO upload via staging buffer, depth attachment, negative-height viewport for Vulkan Y-flip, per-range textured draws, optional MP4 + PPM output |
| `mesh_data.h` | `Vertex { pos[3], normal[3], uv[2] }`, `Material`, `MaterialRange`, and `Mesh { vertices, indices, materials, ranges }` — shared between Vulkan and RISC-V mesh hosts |
| `icosphere.h` | Procedural icosphere generator: subdivision-controlled triangle count (20 → 81920 tris). Used so that load can be varied on demand without external assets |
| `obj_loader.h` | Wavefront OBJ + MTL parser. `v`/`vn`/`vt`/`f` (`pos/uv/normal` triples, `pos//normal` pairs), `mtllib`/`usemtl` with per-material draw ranges, and `map_Kd` diffuse-texture paths resolved relative to the OBJ dir are handled. Face-averaged normals are computed when `vn` is absent from the file; the full line is read for `mtllib` so that filenames with spaces are parsed correctly |

### `test/script/`
| File | Role |
|------|------|
| `bench_common.sh` | Shared library: color codes, QEMU detection, `parse_avg`, `speedup_label` |
| `run_tests.sh` | Unit test runner — each `compiler_tests/` shader is compiled with `irgen_riscv` and the LLVM IR is validated |
| `run_ir_checks.sh` | Codegen-correctness runner — each `codegen_checks/` shader is compiled and the emitted IR is grepped for the expected lowering (coercions, dominance, signedness, …) |
| `run_benchmark_fragment.sh` | Main benchmark: all 13 fragment animations, VK vs RV; a comparison table is printed |
| `run_benchmark_vertex.sh` | Terrain (vertex shader) benchmark: VS/FS/RV sizes + ms/frame table |
| `run_benchmark_mesh.sh` | Indexed-mesh benchmark: the textured "boss" OBJ through the full VBO+IBO pipeline, VK vs RV, sizes + ms/frame table |
| `run_benchmark_compute_blur.sh` | Compute (blur) benchmark |
| `run_benchmark_compute.sh` | Game of Life benchmark (tiny / sweep / animate modes) |
| `run_benchmark_diverge.sh` | Branch-divergence benchmark across resolutions |
| `run_cpu_scaling.sh` | OpenMP thread scaling + Amdahl fit + RVV instruction-count demo |

### `build/` (generated)
| Path | Contents |
|------|----------|
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

A stable C ABI is written by the trampoline emitter into every shader module so
that the compiled shaders can be driven by `pipeline_runtime.cpp` without their
specific signatures being known:

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

Either an indexed mesh (`vbuf` + `indices`) is carried by `PipelineDesc` or, when
both are NULL, the legacy `gl_VertexID`-synthesized geometry path (terrain,
full-screen quads).

---

## Optimization pipeline (RISC-V shaders)

Every `.src` shader is taken through five stages before it becomes a `.o` object:

```
build/riscv/irgen_riscv < shader.src         # parse → AST → raw LLVM IR  (.ll)
llvm-link-18 vs.ll fs.ll                     # merge vertex + fragment modules
opt-18 -O3                                   # GVN, CSE, instcombine, SROA, fast-math, FMA
opt-18 --load-pass-plugin=build/llvm/sincos_opt.so
       -passes='sincos-opt'
                                             # combine sin(X)+cos(X) → sincosf(X,&s,&c)
llc-18 -O3 --fp-contract=fast                # instruction selection → riscv64 machine code (.o)
```

---

## Key design decisions

- **Hand-rolled SPIR-V emitter** — LLVM IR is translated directly to SPIR-V
  bytecode by `emit_spirv_from_ir.h`, with no glslang/llvm-spirv intermediate.
  Pinned to SPIR-V 1.0 for Vulkan 1.0 compatibility, which means the `Uniform`
  storage class with the `BufferBlock` decoration is used for SSBOs rather than
  the newer `StorageBuffer` storage class.
- **Two parallel rendering paths** — for most fragment-only animations geometry
  is synthesized from `gl_VertexID` (no VBOs, no descriptor sets); for the
  indexed-mesh demo real vertex buffers + index buffers are used, by which the
  full vertex input / per-vertex attribute path is exercised on both backends.
- **Same shader source for GPU and CPU** — a single `mesh_vs.src` is compiled
  for both backends by the mesh demo. The flipped Y NDC is handled by Vulkan via
  a negative-height viewport, so the shader's `gl_Position.y` is unchanged.
  A separate `terrain_vs_vk.src` is still kept for terrain because it pre-dates
  the negative-viewport idiom.
- **Push constants for scalar uniforms** — `uniform float uTime` is turned into
  a Vulkan push constant on the GPU side and a plain global on the RISC-V side;
  no descriptor sets are needed for simple uniforms.
- **Direct ffmpeg pipe** — an ffmpeg pipe is opened by both the Vulkan and
  RISC-V hosts before the frame loop and raw RGB is streamed. No intermediate
  PPM frames are written (a single mid-animation PPM is saved for diffing).
- **All build artifacts in `build/`** — compiler binaries, plugins, SPIR-V
  bytecode, and RISC-V intermediates are all placed under `build/`. Only source
  files and test assets are kept in the repo root and `test/`.
- **Unified error logging** — errors are emitted by every host and compiler tool
  via `logError(...)` (or `logErrorFmt("{}", arg)` where formatting is needed)
  from `error_utils.h` / `error_utils_fmt.h`. Errors are prefixed `[ERROR]`
  on `stderr`; stdout is reserved for normal program output.
