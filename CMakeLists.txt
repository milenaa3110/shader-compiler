cmake_minimum_required(VERSION 3.20)
project(GLSLCompiler LANGUAGES CXX VERSION 0.1)

# Toolchain
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # IDE / clangd

# Default warnings
add_compile_options(-Wall -Wextra)

# Use LLVM 18 fallback on LLVM 17
find_package(LLVM 18 CONFIG QUIET)
if(NOT LLVM_FOUND)
    find_package(LLVM 17 CONFIG QUIET)
endif()
if(NOT LLVM_FOUND)
    find_package(LLVM CONFIG REQUIRED)
endif()
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (cmake dir: ${LLVM_CMAKE_DIR})")
list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})

# Versioned LLVM tool binaries
find_program(LLVM_OPT_BIN  NAMES opt-${LLVM_VERSION_MAJOR} opt-18 opt-17 opt REQUIRED)
find_program(LLVM_LLC_BIN  NAMES llc-${LLVM_VERSION_MAJOR} llc-18 llc-17 llc REQUIRED)
find_program(LLVM_LINK_BIN NAMES llvm-link-${LLVM_VERSION_MAJOR} llvm-link-18 llvm-link-17 llvm-link REQUIRED)
message(STATUS "  opt:       ${LLVM_OPT_BIN}")
message(STATUS "  llc:       ${LLVM_LLC_BIN}")
message(STATUS "  llvm-link: ${LLVM_LINK_BIN}")

find_package(fmt REQUIRED)
find_package(Vulkan REQUIRED)

# spirv-headers is a header-only package; we just need the include path for
# main_lib_spirv.cpp (via emit_spirv_from_ir.h).
find_path(SPIRV_HEADERS_INCLUDE_DIR
    NAMES spirv/unified1/spirv.h
    HINTS /usr/include /usr/local/include
    REQUIRED)
message(STATUS "  spirv-headers: ${SPIRV_HEADERS_INCLUDE_DIR}")

# RISC-V cross compiler (optional — only needed for `rv-*` targets).
find_program(CROSS_CXX_BIN NAMES riscv64-linux-gnu-g++ g++)

# clang is used to compile `src/runtime/tex_inline.cpp` to RV LLVM bitcode for
# linking into shader modules (eliminates the per-fragment cross-TU call to
# the bilinear sampler). Required when CROSS_CXX_BIN is set.
find_program(CLANG_CXX_BIN NAMES clang++-${LLVM_VERSION_MAJOR} clang++-18 clang++-17 clang++)
if(CROSS_CXX_BIN AND NOT CLANG_CXX_BIN)
    message(FATAL_ERROR
        "clang++ not found but RISC-V cross-compile is enabled.\n"
        "Install clang-18 (or matching LLVM version) so tex_inline.cpp can\n"
        "be compiled to bitcode for the shader-IR link step.")
endif()
if(CLANG_CXX_BIN)
    message(STATUS "  clang++:   ${CLANG_CXX_BIN}")
endif()
find_program(QEMU_RISCV    NAMES qemu-riscv64-static qemu-riscv64)
if(QEMU_RISCV)
    set(RISCV_SIM "${QEMU_RISCV};-L;/usr/riscv64-linux-gnu" CACHE STRING "")
    message(STATUS "  qemu-riscv64: ${QEMU_RISCV}")
else()
    set(RISCV_SIM "" CACHE STRING "")
    message(STATUS "  qemu-riscv64: not found (rv-* targets will skip)")
endif()

# ── SPMD packet width (f32 lanes) — adapts to the target RVV VLEN ────────────
# "auto": VLEN/32 on a native RISC-V build (probe the vlenb CSR at configure),
# else 4 (QEMU user-mode defaults to VLEN=128). An integer overrides. Baked into
# the emitted .rv IR (kW) and the runtime (PACKET_W); a startup guard aborts on a
# mismatch. Both MUST agree or the SoA stride disagrees and the output is wrong.
set(SHADER_PACKET_WIDTH "auto" CACHE STRING
    "SPMD f32 lane count: 'auto' = VLEN/32 on native RISC-V else 4, or an integer.")
if(SHADER_PACKET_WIDTH STREQUAL "auto")
    set(_PKT_W 4)
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "riscv64" AND CROSS_CXX_BIN)
        file(WRITE ${CMAKE_BINARY_DIR}/_vlenb_probe.cpp
"#include <cstdio>\nint main(){unsigned long v;__asm__ volatile(\"csrr %0, vlenb\":\"=r\"(v));printf(\"%lu\\n\",v*8);return 0;}\n")
        execute_process(
            COMMAND ${CROSS_CXX_BIN} -O2 -static -march=rv64gcv -mabi=lp64d
                    ${CMAKE_BINARY_DIR}/_vlenb_probe.cpp -o ${CMAKE_BINARY_DIR}/_vlenb_probe
            RESULT_VARIABLE _probe_cc)
        if(_probe_cc EQUAL 0)
            execute_process(COMMAND ${CMAKE_BINARY_DIR}/_vlenb_probe
                            OUTPUT_VARIABLE _vlen OUTPUT_STRIP_TRAILING_WHITESPACE
                            RESULT_VARIABLE _probe_run)
            if(_probe_run EQUAL 0 AND _vlen GREATER 0)
                math(EXPR _PKT_W "${_vlen} / 32")
            endif()
        endif()
        file(REMOVE ${CMAKE_BINARY_DIR}/_vlenb_probe.cpp ${CMAKE_BINARY_DIR}/_vlenb_probe)
    endif()
else()
    set(_PKT_W ${SHADER_PACKET_WIDTH})
endif()
if(NOT _PKT_W MATCHES "^(4|8|16)$")
    message(FATAL_ERROR "SHADER_PACKET_WIDTH resolved to '${_PKT_W}' — must be 4, 8, or 16.")
endif()
math(EXPR _PKT_VLEN "${_PKT_W} * 32")
message(STATUS "  SPMD packet width: ${_PKT_W} lanes (target VLEN ${_PKT_VLEN} bits)")
# Surface the width to bench scripts that cross-compile pipeline_runtime.cpp
# outside CMake (they source packet_width.env via bench_common.sh).
file(WRITE ${CMAKE_BINARY_DIR}/packet_width.env "SHADER_PACKET_WIDTH=${_PKT_W}\n")

include(ProcessorCount)
ProcessorCount(NTHREADS)
if(NTHREADS EQUAL 0)
    set(NTHREADS 4)
endif()

# Output subdirs under the build tree. Bench scripts hard-code these as
# build/riscv, build/spirv, build/llvm — keep in sync if changed.
set(RISCV_DIR ${CMAKE_BINARY_DIR}/riscv)
set(SPV_DIR   ${CMAKE_BINARY_DIR}/spirv)
set(LLVM_DIR_ ${CMAKE_BINARY_DIR}/llvm)
file(MAKE_DIRECTORY ${RISCV_DIR} ${SPV_DIR} ${LLVM_DIR_})

# Build-time directory creation: `clean` removes these dirs (see
# ADDITIONAL_CLEAN_FILES below), and CMake only runs file(MAKE_DIRECTORY ...)
# at configure time — so we need a build-time hook to recreate them.
# Every target that writes here depends on _output_dirs.
set(_OUTPUT_DIRS_STAMP ${CMAKE_BINARY_DIR}/.output_dirs.stamp)
add_custom_command(
    OUTPUT ${_OUTPUT_DIRS_STAMP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RISCV_DIR} ${SPV_DIR} ${LLVM_DIR_}
    COMMAND ${CMAKE_COMMAND} -E touch ${_OUTPUT_DIRS_STAMP}
    VERBATIM)
add_custom_target(_output_dirs DEPENDS ${_OUTPUT_DIRS_STAMP})

# ─────────────────────────────────────────────────────────────────────────────
# Shared compiler library — the lexer/parser/AST/codegen, used by all 3 tools
# ─────────────────────────────────────────────────────────────────────────────
add_library(compiler_common STATIC
    src/frontend/lexer/lexer.cpp
    src/frontend/parser/parser.cpp
    src/frontend/sema/sema.cpp
    src/frontend/ast/ast.cpp
    src/frontend/ast/ast_context.cpp
    src/codegen/codegen_state/codegen_state.cpp
    src/codegen/helpers/call_helpers.cpp
    src/codegen/helpers/assignment_helpers.cpp
)
target_include_directories(compiler_common PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(compiler_common PUBLIC ${LLVM_DEFINITIONS})
# LLVM 18 ships as a single shared library on Ubuntu (-lLLVM-18); link it directly.
target_link_libraries(compiler_common PUBLIC LLVM fmt::fmt)
set_property(TARGET compiler_common PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# Standalone lexer unit test — links only lexer.cpp (no LLVM, no parser).
# The buffer-backed Lexer is the one cleanly unit-isolatable component.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(lexer_test test/lexer_test.cpp src/frontend/lexer/lexer.cpp)
target_include_directories(lexer_test PRIVATE ${CMAKE_SOURCE_DIR}/src/frontend)
set_target_properties(lexer_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
enable_testing()
add_test(NAME lexer_test COMMAND lexer_test)

# ─────────────────────────────────────────────────────────────────────────────
# Sema type-stamp test — links compiler_common (parser + sema + AST) and checks
# that SemanticAnalyzer stamps the expected glsl::Type on expression nodes.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(sema_test test/sema_test.cpp)
target_link_libraries(sema_test PRIVATE compiler_common)
target_include_directories(sema_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(sema_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME sema_test COMMAND sema_test)

# ─────────────────────────────────────────────────────────────────────────────
# IR-shape regression tests (FileCheck). Validity (llvm-as in run_tests.sh) can't
# catch *where* an instruction lands — these pin properties that can silently
# regress: && / || short-circuit, and a ternary's widening cast sitting in the
# arm block (PHI dominance). Each test/shaders/codegen_checks/*.src carries its
# own `# CHECK:` directives. Opt-in on FileCheck being installed; runs the
# already-built irgen_riscv (so it must be in the default build, which it is).
# ─────────────────────────────────────────────────────────────────────────────
find_program(FILECHECK_EXE NAMES FileCheck-18 FileCheck)
if(FILECHECK_EXE)
    add_test(NAME ir_checks
             COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_ir_checks.sh --no-build)
    set_tests_properties(ir_checks PROPERTIES ENVIRONMENT "FILECHECK=${FILECHECK_EXE}")
else()
    message(STATUS "FileCheck not found — skipping ir_checks regression test")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Shaders that must be REJECTED, and with which diagnostic. run_tests.sh only
# proves valid shaders compile — nothing proved invalid ones are caught, which
# is how a unary minus on a struct came to abort the compiler and how mat4*ivec4
# came to emit ill-typed IR. Each test/shaders/reject/*.src carries its own
# `# EXPECT-ERROR:` directives. Runs the already-built irgen_riscv.
# ─────────────────────────────────────────────────────────────────────────────
add_test(NAME reject_tests
         COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_reject_tests.sh --no-build)

# ─────────────────────────────────────────────────────────────────────────────
# Executable matrix arithmetic tests. FileCheck pins IR shape and llvm-as proves
# it is well-formed, but neither computes anything — a transposed multiply or a
# mat3 column read at the wrong stride passes both. This links the emitted IR
# with a C++ driver and checks numbers. Needs clang-18; skipped if absent.
# ─────────────────────────────────────────────────────────────────────────────
add_test(NAME matrix_numeric
         COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_matrix_numeric.sh)

# Executable sampler/image test: binds real cube-face / 3D-slice / array-layer /
# storage-image data and checks the runtime samples it (not a placeholder slice).
add_test(NAME sampler_numeric
         COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_sampler_numeric.sh)

# ─────────────────────────────────────────────────────────────────────────────
# Route B Phase 1: SPMD `fs_packet` packetizer regression (emit-only — checks the
# packetizer produces the right width-agnostic vectorized IR shape). Runs the
# already-built irgen_riscv; no runtime/toolchain needed.
# ─────────────────────────────────────────────────────────────────────────────
add_test(NAME packet_test
         COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_packet_test.sh)

# Packet-width RUNTIME gate: links pipeline_runtime.cpp and runs under QEMU to
# check the VLEN banner + the build-width guard (a shader/runtime width mismatch
# must abort loudly, not corrupt the SoA silently). Drives a target so the shader
# object it needs is built first; the script self-skips if cross-cc/QEMU absent.
# ─────────────────────────────────────────────────────────────────────────────
add_custom_target(packet-runtime
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_packet_runtime.sh
    DEPENDS mandelbrot_rv.o
    USES_TERMINAL)
add_test(NAME packet_runtime
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target packet-runtime)

# ─────────────────────────────────────────────────────────────────────────────
# Static SPIR-V vs RISC-V backend comparison (test/script/compare_backends.sh).
# The test drives the `compare-backends` target, which first builds the .spv and
# _rv.o artifacts it reads, then prints the per-shader table — so it is
# self-contained. Needs python3 (the SPIR-V opcode counter); skipped if absent.
# ─────────────────────────────────────────────────────────────────────────────
find_program(PYTHON3_EXE NAMES python3)
if(PYTHON3_EXE)
    add_test(NAME compare_backends
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target compare-backends)
else()
    message(STATUS "python3 not found — skipping compare_backends test")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# SPIR-V validation. The emitter fails silently by design (unsupported types
# return id 0, unhandled constants become OpConstantNull, the binary-op switch
# defaults to FAdd), so a malformed module reaches disk with a zero exit code —
# nothing else in the suite would notice. Drives the `spirv-validate` target,
# which builds the .spv it reads, so it is self-contained. Needs spirv-val.
# ─────────────────────────────────────────────────────────────────────────────
find_program(SPIRV_VAL_EXE NAMES spirv-val)
if(SPIRV_VAL_EXE)
    add_test(NAME spirv_validate
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target spirv-validate)
else()
    message(STATUS "spirv-val not found — skipping spirv_validate test")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Parser fuzzer (libFuzzer). Opt-in: requires clang with -fsanitize=fuzzer.
# Built only when CMAKE_CXX_COMPILER is clang and ENABLE_FUZZER is ON.
#   cmake -B build -DENABLE_FUZZER=ON -DCMAKE_CXX_COMPILER=clang++-18
#   cmake --build build --target parser_fuzz
#   ./build/parser_fuzz -max_total_time=30
# Build-isolated from compiler_common so we get a clean instrumented build
# (libFuzzer doesn't mix with non-instrumented TUs in the same binary).
# ─────────────────────────────────────────────────────────────────────────────
option(ENABLE_FUZZER "Build libFuzzer-based parser fuzzer (requires clang)" OFF)
if(ENABLE_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
            "ENABLE_FUZZER=ON requires Clang (have ${CMAKE_CXX_COMPILER_ID}). "
            "Re-run cmake with -DCMAKE_CXX_COMPILER=clang++-18")
    endif()
    add_executable(parser_fuzz
        test/fuzz/parser_fuzz.cpp
        src/frontend/lexer/lexer.cpp
        src/frontend/parser/parser.cpp
        src/frontend/sema/sema.cpp
        src/frontend/ast/ast.cpp
        src/frontend/ast/ast_context.cpp
        src/codegen/codegen_state/codegen_state.cpp
        src/codegen/helpers/call_helpers.cpp
        src/codegen/helpers/assignment_helpers.cpp
    )
    target_include_directories(parser_fuzz PRIVATE
        ${CMAKE_SOURCE_DIR} ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(parser_fuzz PRIVATE ${LLVM_DEFINITIONS})
    target_compile_options(parser_fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined -g -O1 -fno-omit-frame-pointer)
    target_link_options(parser_fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined)
    target_link_libraries(parser_fuzz PRIVATE LLVM fmt::fmt)
    set_target_properties(parser_fuzz PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    message(STATUS "  parser_fuzz: enabled (clang ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Compiler tool binaries
# ─────────────────────────────────────────────────────────────────────────────
add_executable(irgen_riscv src/codegen/emit/main_lib_riscv.cpp)
target_link_libraries(irgen_riscv PRIVATE compiler_common)
target_include_directories(irgen_riscv PRIVATE ${CMAKE_SOURCE_DIR}/src)
# Bakes kW into the emitted .rv IR — must match the runtime's PACKET_W.
target_compile_definitions(irgen_riscv PRIVATE SHADER_PACKET_WIDTH=${_PKT_W})
set_target_properties(irgen_riscv PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RISCV_DIR})
add_dependencies(irgen_riscv _output_dirs)

add_executable(irgen_spirv src/codegen/emit/main_lib_spirv.cpp)
target_link_libraries(irgen_spirv PRIVATE compiler_common)
target_include_directories(irgen_spirv PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${SPIRV_HEADERS_INCLUDE_DIR})
set_target_properties(irgen_spirv PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SPV_DIR})
add_dependencies(irgen_spirv _output_dirs)

# ─────────────────────────────────────────────────────────────────────────────
# LLVM pass plugin: sincos_opt
# Combines llvm.sin.f32(X)+llvm.cos.f32(X) into a single sincosf call after
# opt -O3 has unified duplicate sin/cos arguments via GVN.
# ─────────────────────────────────────────────────────────────────────────────
add_library(sincos_opt MODULE src/passes/sincos_opt.cpp)
target_include_directories(sincos_opt PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(sincos_opt PRIVATE ${LLVM_DEFINITIONS})
target_compile_features(sincos_opt PRIVATE cxx_std_17)  # LLVM pass API uses C++17
set_target_properties(sincos_opt PROPERTIES
    PREFIX ""           # produce sincos_opt.so, not libsincos_opt.so
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY ${LLVM_DIR_})
add_dependencies(sincos_opt _output_dirs)
set(SINCOS_PLUGIN_PATH ${LLVM_DIR_}/sincos_opt.so)

# ─────────────────────────────────────────────────────────────────────────────
# Vulkan host binaries (renderers + benchmark hosts)
# ─────────────────────────────────────────────────────────────────────────────
function(add_vulkan_host name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Vulkan::Vulkan fmt::fmt)
    target_compile_options(${name} PRIVATE -O2 -Wno-missing-field-initializers)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SPV_DIR})
    add_dependencies(${name} _output_dirs)
endfunction()

add_vulkan_host(spirv_vulkan_host         test/vk_host/vk_host_fragment.cpp)
add_vulkan_host(spirv_vulkan_compute_host test/vk_host/vk_host_compute_blur.cpp)
add_vulkan_host(spirv_vulkan_life_host    test/vk_host/vk_host_compute.cpp)
add_vulkan_host(spirv_vulkan_mesh_host    test/vk_host/vk_host_mesh.cpp)
add_vulkan_host(spirv_vulkan_texture_host test/vk_host/vk_host_texture.cpp)
add_vulkan_host(spirv_vulkan_volume_host  test/vk_host/vk_host_volume.cpp)
add_vulkan_host(spirv_vulkan_volume_cs_host test/vk_host/vk_host_volume_cs.cpp)

# GPU Vulkan under WSL2. The NVIDIA WSL driver projects only CUDA/D3D12 into
# Linux — there is no native Vulkan ICD and no /dev/nvidia*, so vkEnumerate‑
# PhysicalDevices sees lavapipe (CPU) and nothing else. Mesa's Dozen driver
# implements Vulkan over D3D12, which is what WSL does expose via /dev/dxg.
# Point DZN_ICD at its manifest to make the vk-* run targets use the real GPU:
#   cmake -B build -DDZN_ICD=/opt/mesa-dzn/share/vulkan/icd.d/dzn_icd.x86_64.json
# Left empty, the run targets use whatever the loader picks by default.
set(DZN_ICD "" CACHE FILEPATH
    "dzn_icd.x86_64.json enabling GPU Vulkan over D3D12 (WSL2)")
if(DZN_ICD)
    set(VK_RUN_ENV ${CMAKE_COMMAND} -E env
        "VK_ICD_FILENAMES=${DZN_ICD}" "LD_LIBRARY_PATH=/usr/lib/wsl/lib")
    message(STATUS "  Vulkan ICD override: ${DZN_ICD}")
else()
    set(VK_RUN_ENV "")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Shader compilation: helper functions
# ─────────────────────────────────────────────────────────────────────────────
set(ANIM_SHADER_DIR ${CMAKE_SOURCE_DIR}/test/shaders/animations)
set(PIPELINE_DIR    ${CMAKE_SOURCE_DIR}/src/runtime)
set(RV_HOST_SRC     ${CMAKE_SOURCE_DIR}/test/rv_host/rv_host_fragment.cpp)
set(SCENE_VS_SRC    ${CMAKE_SOURCE_DIR}/test/shaders/pipeline/scene_vs.src)

# tex_inline.bc — bilinear sampler compiled to RV LLVM bitcode for linking
# into shader modules. With both functions marked `always_inline`, opt -O3
# substitutes the body at every fragment-shader call site, eliminating the
# per-fragment cross-TU function-call overhead.
if(CLANG_CXX_BIN)
    set(TEX_INLINE_SRC ${PIPELINE_DIR}/tex_inline.cpp)
    set(TEX_INLINE_BC  ${LLVM_DIR_}/tex_inline.bc)
    add_custom_command(
        OUTPUT  ${TEX_INLINE_BC}
        COMMAND ${CLANG_CXX_BIN} -O1 -emit-llvm -c -std=c++20
                --target=riscv64-linux-gnu
                -march=rv64gcv -mabi=lp64d
                -I${PIPELINE_DIR}
                ${TEX_INLINE_SRC}
                -o ${TEX_INLINE_BC}
        DEPENDS ${TEX_INLINE_SRC} ${PIPELINE_DIR}/tex_inline.h _output_dirs
        COMMENT "[bc]  tex_inline.bc"
        VERBATIM)
    add_custom_target(tex_inline.bc DEPENDS ${TEX_INLINE_BC})
endif()

# Compile a vertex shader (.src → .vert.spv) using the irgen_spirv emitter.
function(spv_compile_vertex name srcfile)
    set(out ${SPV_DIR}/${name}.vert.spv)
    add_custom_command(
        OUTPUT  ${out}
        COMMAND $<TARGET_FILE:irgen_spirv> ${out} < ${srcfile}
        DEPENDS ${srcfile} irgen_spirv
        COMMENT "[spv] ${name}.vert.spv"
        VERBATIM)
    add_custom_target(${name}.vert.spv DEPENDS ${out})
    set_property(GLOBAL APPEND PROPERTY SPV_MODULES ${name}.vert.spv)
endfunction()

# Compile a fragment shader (.src → .frag.spv).
function(spv_compile_fragment name srcfile)
    set(out ${SPV_DIR}/${name}.frag.spv)
    add_custom_command(
        OUTPUT  ${out}
        COMMAND $<TARGET_FILE:irgen_spirv> ${out} < ${srcfile}
        DEPENDS ${srcfile} irgen_spirv
        COMMENT "[spv] ${name}.frag.spv"
        VERBATIM)
    add_custom_target(${name}.frag.spv DEPENDS ${out})
    set_property(GLOBAL APPEND PROPERTY SPV_MODULES ${name}.frag.spv)
endfunction()

# Compile a compute shader (.src → .comp.spv).
function(spv_compile_compute name srcfile)
    set(out ${SPV_DIR}/${name}.comp.spv)
    add_custom_command(
        OUTPUT  ${out}
        COMMAND $<TARGET_FILE:irgen_spirv> ${out} < ${srcfile}
        DEPENDS ${srcfile} irgen_spirv
        COMMENT "[spv] ${name}.comp.spv"
        VERBATIM)
    add_custom_target(${name}.comp.spv DEPENDS ${out})
    set_property(GLOBAL APPEND PROPERTY SPV_MODULES ${name}.comp.spv)
endfunction()

# RISC-V single-shader path (used for the per-anim VS+FS pipeline).
# Produces:
#   ${RISCV_DIR}/<name>_rv.ll
#   ${RISCV_DIR}/<name>_rv.o   (after opt+sincos+llc with RVV)
#   ${RISCV_DIR}/<name>.rv     (cross-compiled host binary)
function(rv_compile_animation name)
    set(srcfile  ${ANIM_SHADER_DIR}/${name}_fs.src)
    set(ll       ${RISCV_DIR}/${name}_rv.ll)
    set(vsll     ${RISCV_DIR}/${name}_vs_rv.ll)
    set(fsll     ${RISCV_DIR}/${name}_fs_rv.ll)
    set(gvnll    ${RISCV_DIR}/${name}_rv.gvn.ll)
    set(optll    ${RISCV_DIR}/${name}_rv.opt.ll)
    set(obj      ${RISCV_DIR}/${name}_rv.o)
    set(rvbin    ${RISCV_DIR}/${name}.rv)

    add_custom_command(
        OUTPUT  ${ll}
        BYPRODUCTS ${vsll} ${fsll}
        COMMAND $<TARGET_FILE:irgen_riscv> ${vsll} < ${SCENE_VS_SRC}
        COMMAND $<TARGET_FILE:irgen_riscv> ${fsll} < ${srcfile}
        # Link in tex_inline.bc so calls to __tex2d_sample / __tex_lookup get
        # inlined by opt -O3 (both are marked always_inline). Harmless for
        # shaders that don't sample textures: bodies get DCE'd.
        COMMAND ${LLVM_LINK_BIN} ${vsll} ${fsll} ${TEX_INLINE_BC} -S -o ${ll}
        DEPENDS ${srcfile} ${SCENE_VS_SRC} irgen_riscv ${TEX_INLINE_BC}
        COMMENT "[rv]  ${name}_rv.ll"
        VERBATIM)
    add_custom_target(${name}_rv.ll DEPENDS ${ll})

    add_custom_command(
        OUTPUT  ${obj}
        BYPRODUCTS ${gvnll} ${optll}
        COMMAND ${LLVM_OPT_BIN} -O3 --enable-unsafe-fp-math --fp-contract=fast -S
                ${ll} -o ${gvnll}
        COMMAND ${LLVM_OPT_BIN} -load-pass-plugin=${SINCOS_PLUGIN_PATH}
                -passes=sincos-opt,mem2reg,instcombine -S ${gvnll} -o ${optll}
        COMMAND ${LLVM_LLC_BIN} -O3 --fp-contract=fast -filetype=obj
                -relocation-model=pic -mtriple=riscv64-unknown-linux-gnu
                -mattr=+m,+a,+f,+d,+v ${optll} -o ${obj}
        DEPENDS ${ll} sincos_opt
        COMMENT "[rv]  ${name}_rv.o (opt + sincos-opt + llc)"
        VERBATIM)
    add_custom_target(${name}_rv.o DEPENDS ${obj})

    if(CROSS_CXX_BIN)
        add_custom_command(
            OUTPUT  ${rvbin}
            COMMAND ${CROSS_CXX_BIN} -std=c++20 -O3 -static -fopenmp
                    -march=rv64gcv -mabi=lp64d
                    -I${PIPELINE_DIR}
                    -DANIM_NAME="${name}" -DNFRAMES=300 -DWIDTH=512 -DHEIGHT=512
                    -DSHADER_PACKET_WIDTH=${_PKT_W}
                    ${RV_HOST_SRC} ${PIPELINE_DIR}/pipeline_runtime.cpp
                    ${obj} -o ${rvbin}
            DEPENDS ${obj} ${PIPELINE_DIR}/pipeline_runtime.cpp ${RV_HOST_SRC}
            COMMENT "[rv]  ${name}.rv"
            VERBATIM)
        add_custom_target(${name}.rv DEPENDS ${rvbin})
    endif()
endfunction()

# ─────────────────────────────────────────────────────────────────────────────
# Animation list
# ─────────────────────────────────────────────────────────────────────────────
set(ANIMATIONS_PROC mandelbrot julia voronoi waves tunnel ripple galaxy fire
                    reaction cellular earth scene3d diverge city ocean matrix)
set(ANIMATIONS ${ANIMATIONS_PROC} texture_test)

# Vulkan vertex shader for the full-screen quad.
spv_compile_vertex(quad ${ANIM_SHADER_DIR}/quad_vs.src)

# Matrix demo — the shader that makes spirv-val exercise the OpTypeMatrix path
# (uniform/constant matrices, matrix builtins, whole-matrix arithmetic).
spv_compile_vertex(matdemo ${ANIM_SHADER_DIR}/matdemo_vs.src)

# Per-animation Vulkan fragment + RISC-V object/binary.
foreach(anim ${ANIMATIONS_PROC})
    spv_compile_fragment(${anim} ${ANIM_SHADER_DIR}/${anim}_fs.src)
    rv_compile_animation(${anim})

    # Render targets matching the old Makefile's vk-<name> / rv-<name>.
    add_custom_target(vk-${anim}
        COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_host>
                ${SPV_DIR}/quad.vert.spv ${SPV_DIR}/${anim}.frag.spv
                ${anim} 300 512 512
        DEPENDS spirv_vulkan_host quad.vert.spv ${anim}.frag.spv
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM)
    if(CROSS_CXX_BIN)
        add_custom_target(rv-${anim}
            COMMAND ${CMAKE_COMMAND} -E env "OMP_NUM_THREADS=${NTHREADS}" "SHADER_PACKET=1"
                    ${RISCV_SIM} ${RISCV_DIR}/${anim}.rv
            DEPENDS ${anim}.rv
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            VERBATIM)
    endif()
endforeach()

# texture_test only has a RISC-V variant in the old Makefile.
rv_compile_animation(texture_test)

# Compute shaders (Game of Life + Gaussian blur).
spv_compile_compute(life  ${ANIM_SHADER_DIR}/life_cs.src)
spv_compile_compute(blur  ${ANIM_SHADER_DIR}/blur_cs.src)

# RISC-V compute objects (no scene_vs.src — these are pure compute).
foreach(cs life blur)
    set(srcfile ${ANIM_SHADER_DIR}/${cs}_cs.src)
    set(ll      ${RISCV_DIR}/${cs}_cs_rv.ll)
    set(gvnll   ${RISCV_DIR}/${cs}_cs_rv.gvn.ll)
    set(optll   ${RISCV_DIR}/${cs}_cs_rv.opt.ll)
    set(obj     ${RISCV_DIR}/${cs}_cs_rv.o)

    add_custom_command(
        OUTPUT  ${ll}
        COMMAND $<TARGET_FILE:irgen_riscv> ${ll} < ${srcfile}
        DEPENDS ${srcfile} irgen_riscv
        COMMENT "[rv]  ${cs}_cs_rv.ll"
        VERBATIM)
    add_custom_target(${cs}_cs_rv.ll DEPENDS ${ll})

    add_custom_command(
        OUTPUT  ${obj}
        BYPRODUCTS ${gvnll} ${optll}
        COMMAND ${LLVM_OPT_BIN} -O3 --enable-unsafe-fp-math --fp-contract=fast -S
                ${ll} -o ${gvnll}
        COMMAND ${LLVM_OPT_BIN} -load-pass-plugin=${SINCOS_PLUGIN_PATH}
                -passes=sincos-opt,mem2reg,instcombine -S ${gvnll} -o ${optll}
        COMMAND ${LLVM_LLC_BIN} -O3 --fp-contract=fast -filetype=obj
                -relocation-model=pic -mtriple=riscv64-unknown-linux-gnu
                -mattr=+m,+a,+f,+d,+v ${optll} -o ${obj}
        DEPENDS ${ll} sincos_opt
        COMMENT "[rv]  ${cs}_cs_rv.o"
        VERBATIM)
    add_custom_target(${cs}_cs_rv.o DEPENDS ${obj})
endforeach()

# Texture-sampling fragment shader (uses sampler2D — Vulkan only).
spv_compile_fragment(texture_test ${ANIM_SHADER_DIR}/texture_test_gpu_fs.src)

# Sampler/image coverage for spirv-val: cube/3D/array/LOD sampling + storage images.
spv_compile_fragment(cubemap ${ANIM_SHADER_DIR}/cubemap_fs.src)
spv_compile_compute(imagestore ${ANIM_SHADER_DIR}/imagestore_cs.src)
# texlayer_fs: cube + 3D + array + 2D sampling AND an image2D load/store in one
# fragment shader — the widest single sampler/image spirv-val fixture.
spv_compile_fragment(texlayer ${ANIM_SHADER_DIR}/texlayer_fs.src)

# ─────────────────────────────────────────────────────────────────────────────
# RISC-V layered-texture / storage-image RENDER host. The only caller of the
# public bind_texture_layered() / bind_image() APIs: it binds a cube, a 3D volume,
# a 2D array, a plain 2D texture, and a read/write image2D, renders texlayer_fs,
# and verifies both the pixel color and the image round-trip. Runs under QEMU as
# the `texlayer_rv` ctest; self-skips if the toolchain is absent.
#
# IMPORTANT — reduced opt on purpose. `opt -O3` MISCOMPILES this shader on the
# RISC-V backend: inlining the always_inline sampler bodies (which write `float*
# out`, read back as <4 x float>, under vscale_range) into fs_main yields a
# NULL-deref / wrong-pixel render (LLVM-18 backend bug; scalar and vector, even at
# llc -O0). The runtime itself is correct — sampler_numeric + the x86 path pass,
# and a NON-inlining opt pipeline renders bit-exactly. So layered/image render
# shaders use a curated pipeline (mem2reg/sroa/gvn/dse/… — NO inliner) + llc -O3.
# The 2D-only render shaders keep the aggressive -O3 (they don't hit the bug).
# ─────────────────────────────────────────────────────────────────────────────
if(CROSS_CXX_BIN)
    set(_tl_vs  ${RISCV_DIR}/texlayer_vs_rv.ll)
    set(_tl_fs  ${RISCV_DIR}/texlayer_fs_rv.ll)
    set(_tl_ll  ${RISCV_DIR}/texlayer_rv.ll)
    set(_tl_opt ${RISCV_DIR}/texlayer_rv.opt.ll)
    set(_tl_o   ${RISCV_DIR}/texlayer_rv.o)
    set(_tl_bin ${RISCV_DIR}/texlayer.rv)
    set(_tl_host ${CMAKE_SOURCE_DIR}/test/rv_host/rv_host_texlayer.cpp)

    add_custom_command(
        OUTPUT  ${_tl_ll}
        BYPRODUCTS ${_tl_vs} ${_tl_fs}
        COMMAND $<TARGET_FILE:irgen_riscv> ${_tl_vs} < ${SCENE_VS_SRC}
        COMMAND $<TARGET_FILE:irgen_riscv> ${_tl_fs} < ${ANIM_SHADER_DIR}/texlayer_fs.src
        # Link tex_inline.bc so __tex*/__image* resolve against g_tex/g_img.
        COMMAND ${LLVM_LINK_BIN} ${_tl_vs} ${_tl_fs} ${TEX_INLINE_BC} -S -o ${_tl_ll}
        DEPENDS ${SCENE_VS_SRC} ${ANIM_SHADER_DIR}/texlayer_fs.src irgen_riscv ${TEX_INLINE_BC}
        COMMENT "[rv]  texlayer_rv.ll"
        VERBATIM)

    add_custom_command(
        OUTPUT  ${_tl_o}
        BYPRODUCTS ${_tl_opt}
        # Curated NON-inlining pipeline (see the note above) — everything but the
        # inliner, so the sampler bodies stay out-of-line and llc compiles them
        # correctly. Renders bit-identically to the safe reference.
        COMMAND ${LLVM_OPT_BIN}
                -passes=function\(mem2reg,sroa,early-cse,instcombine,gvn,dse,simplifycfg,adce\)
                -S ${_tl_ll} -o ${_tl_opt}
        COMMAND ${LLVM_LLC_BIN} -O3 --fp-contract=fast -filetype=obj
                -relocation-model=pic -mtriple=riscv64-unknown-linux-gnu
                -mattr=+m,+a,+f,+d,+v ${_tl_opt} -o ${_tl_o}
        DEPENDS ${_tl_ll}
        COMMENT "[rv]  texlayer_rv.o (reduced opt — no inliner, LLVM -O3 sampler bug)"
        VERBATIM)

    add_custom_command(
        OUTPUT  ${_tl_bin}
        COMMAND ${CROSS_CXX_BIN} -std=c++20 -O2 -static -fopenmp
                -march=rv64gcv -mabi=lp64d -DSHADER_PACKET_WIDTH=${_PKT_W}
                ${_tl_host} ${PIPELINE_DIR}/pipeline_runtime.cpp ${_tl_o} -o ${_tl_bin}
        DEPENDS ${_tl_o} ${PIPELINE_DIR}/pipeline_runtime.cpp ${_tl_host}
        COMMENT "[rv]  texlayer.rv"
        VERBATIM)
    add_custom_target(texlayer.rv DEPENDS ${_tl_bin})

    add_custom_target(texlayer-rv-run
        COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_rv_host.sh ${_tl_bin} texlayer-rv
        DEPENDS ${_tl_bin}
        USES_TERMINAL)
    add_test(NAME texlayer_rv
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target texlayer-rv-run)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Volumetric sampler3D animation — the only ANIMATED consumer of the 3D sampler.
# texlayer proves the layered runtime is correct on a 1x1 solid volume; this
# renders a real 64^3 volume over a marching loop, so trilinear filtering and the
# slice interpolation show up as picture rather than as a single asserted pixel.
#
# Uses the same reduced NON-inlining opt pipeline as texlayer, for the same
# reason: `opt -O3` miscompiles the inlined non-2D sampler bodies on the RISC-V
# backend (LLVM-18). See the note above the texlayer block.
# ─────────────────────────────────────────────────────────────────────────────
spv_compile_fragment(volume ${ANIM_SHADER_DIR}/volume_fs.src)

# GPU render: uploads the same baked volume as a VK_IMAGE_TYPE_3D and binds it
# to the combined image sampler the shader declares at set 0 binding 0.
add_custom_target(vk-volume
    COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_volume_host>
            ${SPV_DIR}/quad.vert.spv ${SPV_DIR}/volume.frag.spv
            volume 90 256 256
    DEPENDS spirv_vulkan_volume_host quad.vert.spv volume.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)

if(CROSS_CXX_BIN)
    set(_vol_vs   ${RISCV_DIR}/volume_vs_rv.ll)
    set(_vol_fs   ${RISCV_DIR}/volume_fs_rv.ll)
    set(_vol_ll   ${RISCV_DIR}/volume_rv.ll)
    set(_vol_opt  ${RISCV_DIR}/volume_rv.opt.ll)
    set(_vol_o    ${RISCV_DIR}/volume_rv.o)
    set(_vol_bin  ${RISCV_DIR}/volume.rv)
    set(_vol_host ${CMAKE_SOURCE_DIR}/test/rv_host/rv_host_volume.cpp)

    add_custom_command(
        OUTPUT  ${_vol_ll}
        BYPRODUCTS ${_vol_vs} ${_vol_fs}
        COMMAND $<TARGET_FILE:irgen_riscv> ${_vol_vs} < ${SCENE_VS_SRC}
        COMMAND $<TARGET_FILE:irgen_riscv> ${_vol_fs} < ${ANIM_SHADER_DIR}/volume_fs.src
        COMMAND ${LLVM_LINK_BIN} ${_vol_vs} ${_vol_fs} ${TEX_INLINE_BC} -S -o ${_vol_ll}
        DEPENDS ${SCENE_VS_SRC} ${ANIM_SHADER_DIR}/volume_fs.src irgen_riscv ${TEX_INLINE_BC}
        COMMENT "[rv]  volume_rv.ll"
        VERBATIM)

    add_custom_command(
        OUTPUT  ${_vol_o}
        BYPRODUCTS ${_vol_opt}
        COMMAND ${LLVM_OPT_BIN}
                -passes=function\(mem2reg,sroa,early-cse,instcombine,gvn,dse,simplifycfg,adce\)
                -S ${_vol_ll} -o ${_vol_opt}
        COMMAND ${LLVM_LLC_BIN} -O3 --fp-contract=fast -filetype=obj
                -relocation-model=pic -mtriple=riscv64-unknown-linux-gnu
                -mattr=+m,+a,+f,+d,+v ${_vol_opt} -o ${_vol_o}
        DEPENDS ${_vol_ll}
        COMMENT "[rv]  volume_rv.o (reduced opt — no inliner, LLVM -O3 sampler bug)"
        VERBATIM)

    add_custom_command(
        OUTPUT  ${_vol_bin}
        COMMAND ${CROSS_CXX_BIN} -std=c++20 -O2 -static -fopenmp
                -march=rv64gcv -mabi=lp64d -DSHADER_PACKET_WIDTH=${_PKT_W}
                -DANIM_NAME="volume" -DNFRAMES=90 -DWIDTH=256 -DHEIGHT=256
                ${_vol_host} ${PIPELINE_DIR}/pipeline_runtime.cpp ${_vol_o} -o ${_vol_bin}
        DEPENDS ${_vol_o} ${PIPELINE_DIR}/pipeline_runtime.cpp ${_vol_host}
        COMMENT "[rv]  volume.rv"
        VERBATIM)
    add_custom_target(volume.rv DEPENDS ${_vol_bin})

    # Scalar render: the packetizer has no vec3-sampler path, so a packet build
    # would fall back per-pixel anyway.
    add_custom_target(rv-volume
        COMMAND ${CMAKE_COMMAND} -E env "OMP_NUM_THREADS=${NTHREADS}"
                ${RISCV_SIM} ${RISCV_DIR}/volume.rv
        DEPENDS volume.rv
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Compute variant of the same ray-march: samples the 3D texture and writes the
# frame through imageStore rather than rasterizing it. This is the shape
# volumetrics take in practice, and it is the only sampler3D read from a compute
# stage in the tree — texlayer covers sampler3D in fragment and imagestore_cs
# covers imageStore without sampling.
#
# Unlike life/blur, this one links tex_inline.bc (it calls __tex3d_sample_lod /
# __image2d_store) and therefore inherits the same no-inliner opt requirement as
# the other layered-sampler shaders.
# ─────────────────────────────────────────────────────────────────────────────
spv_compile_compute(volume ${ANIM_SHADER_DIR}/volume_cs.src)

add_custom_target(vk-volume-cs
    COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_volume_cs_host>
            ${SPV_DIR}/volume.comp.spv volume_cs 90 256 256
    DEPENDS spirv_vulkan_volume_cs_host volume.comp.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)

if(CROSS_CXX_BIN)
    set(_vcs_raw  ${RISCV_DIR}/volume_cs_raw_rv.ll)
    set(_vcs_ll   ${RISCV_DIR}/volume_cs_rv.ll)
    set(_vcs_opt  ${RISCV_DIR}/volume_cs_rv.opt.ll)
    set(_vcs_o    ${RISCV_DIR}/volume_cs_rv.o)
    set(_vcs_bin  ${RISCV_DIR}/volume_cs.rv)
    set(_vcs_host ${CMAKE_SOURCE_DIR}/test/rv_host/rv_host_volume_cs.cpp)

    add_custom_command(
        OUTPUT  ${_vcs_ll}
        BYPRODUCTS ${_vcs_raw}
        COMMAND $<TARGET_FILE:irgen_riscv> ${_vcs_raw} < ${ANIM_SHADER_DIR}/volume_cs.src
        COMMAND ${LLVM_LINK_BIN} ${_vcs_raw} ${TEX_INLINE_BC} -S -o ${_vcs_ll}
        DEPENDS ${ANIM_SHADER_DIR}/volume_cs.src irgen_riscv ${TEX_INLINE_BC}
        COMMENT "[rv]  volume_cs_rv.ll"
        VERBATIM)

    add_custom_command(
        OUTPUT  ${_vcs_o}
        BYPRODUCTS ${_vcs_opt}
        COMMAND ${LLVM_OPT_BIN}
                -passes=function\(mem2reg,sroa,early-cse,instcombine,gvn,dse,simplifycfg,adce\)
                -S ${_vcs_ll} -o ${_vcs_opt}
        COMMAND ${LLVM_LLC_BIN} -O3 --fp-contract=fast -filetype=obj
                -relocation-model=pic -mtriple=riscv64-unknown-linux-gnu
                -mattr=+m,+a,+f,+d,+v ${_vcs_opt} -o ${_vcs_o}
        DEPENDS ${_vcs_ll}
        COMMENT "[rv]  volume_cs_rv.o (reduced opt — no inliner, LLVM -O3 sampler bug)"
        VERBATIM)

    add_custom_command(
        OUTPUT  ${_vcs_bin}
        COMMAND ${CROSS_CXX_BIN} -std=c++20 -O2 -static -fopenmp
                -march=rv64gcv -mabi=lp64d -DSHADER_PACKET_WIDTH=${_PKT_W}
                -DANIM_NAME="volume_cs" -DNFRAMES=90 -DWIDTH=256 -DHEIGHT=256
                ${_vcs_host} ${_vcs_o} -o ${_vcs_bin}
        DEPENDS ${_vcs_o} ${_vcs_host}
        COMMENT "[rv]  volume_cs.rv"
        VERBATIM)
    add_custom_target(volume_cs.rv DEPENDS ${_vcs_bin})

    add_custom_target(rv-volume-cs
        COMMAND ${CMAKE_COMMAND} -E env "OMP_NUM_THREADS=${NTHREADS}"
                ${RISCV_SIM} ${RISCV_DIR}/volume_cs.rv
        DEPENDS volume_cs.rv
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM)
endif()

# Terrain mesh demo: Vulkan-flavoured VS (negated Y for NDC) + shared FS.
spv_compile_vertex(terrain ${ANIM_SHADER_DIR}/terrain_vs_vk.src)
spv_compile_fragment(terrain ${ANIM_SHADER_DIR}/terrain_fs.src)

# Indexed-mesh demo: VS reads aPos/aNormal from VBO, FS does Lambert lighting.
# Same VS source for GPU + CPU — Vulkan host uses a negative-height viewport
# to handle Vulkan's flipped Y NDC convention.
spv_compile_vertex(mesh ${ANIM_SHADER_DIR}/mesh_vs.src)
spv_compile_fragment(mesh ${ANIM_SHADER_DIR}/mesh_fs.src)

# Default `vk-mesh` runs an icosphere with subdivision level 3 (1280 triangles).
# Override with: cmake --build build --target vk-mesh-bunny  (loads test/assets/bunny.obj)
add_custom_target(vk-mesh
    COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            mesh 300 icosphere:3
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
add_custom_target(vk-mesh-hi
    COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            mesh_hi 300 icosphere:5
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
add_custom_target(vk-mesh-bunny
    COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            bunny 300 ${CMAKE_SOURCE_DIR}/test/assets/bunny.obj
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
add_custom_target(vk-mesh-jeep
    COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            jeep 300 ${CMAKE_SOURCE_DIR}/test/assets/Jeep_Renegade_2016.obj
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
add_custom_target(vk-mesh-teddy
    COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            teddy 300 ${CMAKE_SOURCE_DIR}/test/assets/teddy-bear/teddy-bear.obj
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
# OBJ filename has a space ("Rumba Dancing.obj") — quote at expansion time so
# the shell gets one argument. obj_loader resolves map_Kd relative to the OBJ
# dir, so the texture is picked up automatically.
add_custom_target(vk-mesh-boss
    COMMAND ${VK_RUN_ENV} $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            boss 300 "${CMAKE_SOURCE_DIR}/test/assets/boss/Rumba Dancing.obj"
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)

# RISC-V mesh pipeline: link mesh_vs.src (no Y flip) + mesh_fs.src into mesh_rv.o.
set(_mesh_ll    ${RISCV_DIR}/mesh_rv.ll)
set(_mesh_vsll  ${RISCV_DIR}/mesh_vs_rv.ll)
set(_mesh_fsll  ${RISCV_DIR}/mesh_fs_rv.ll)
set(_mesh_gvnll ${RISCV_DIR}/mesh_rv.gvn.ll)
set(_mesh_optll ${RISCV_DIR}/mesh_rv.opt.ll)
set(_mesh_o     ${RISCV_DIR}/mesh_rv.o)

add_custom_command(
    OUTPUT  ${_mesh_ll}
    BYPRODUCTS ${_mesh_vsll} ${_mesh_fsll}
    COMMAND $<TARGET_FILE:irgen_riscv> ${_mesh_vsll} < ${ANIM_SHADER_DIR}/mesh_vs.src
    COMMAND $<TARGET_FILE:irgen_riscv> ${_mesh_fsll} < ${ANIM_SHADER_DIR}/mesh_fs.src
    # Link tex_inline.bc into the shader module so __tex2d_sample inlines.
    COMMAND ${LLVM_LINK_BIN} ${_mesh_vsll} ${_mesh_fsll} ${TEX_INLINE_BC} -S -o ${_mesh_ll}
    DEPENDS ${ANIM_SHADER_DIR}/mesh_vs.src ${ANIM_SHADER_DIR}/mesh_fs.src
            irgen_riscv ${TEX_INLINE_BC}
    VERBATIM)
add_custom_target(mesh_rv.ll DEPENDS ${_mesh_ll})

add_custom_command(
    OUTPUT  ${_mesh_o}
    BYPRODUCTS ${_mesh_gvnll} ${_mesh_optll}
    COMMAND ${LLVM_OPT_BIN} -O3 --enable-unsafe-fp-math --fp-contract=fast -S
            ${_mesh_ll} -o ${_mesh_gvnll}
    COMMAND ${LLVM_OPT_BIN} -load-pass-plugin=${SINCOS_PLUGIN_PATH}
            -passes=sincos-opt,mem2reg,instcombine -S ${_mesh_gvnll} -o ${_mesh_optll}
    COMMAND ${LLVM_LLC_BIN} -O3 --fp-contract=fast -filetype=obj
            -relocation-model=pic -mtriple=riscv64-unknown-linux-gnu
            -mattr=+m,+a,+f,+d,+v ${_mesh_optll} -o ${_mesh_o}
    DEPENDS ${_mesh_ll} sincos_opt
    VERBATIM)
add_custom_target(mesh_rv.o DEPENDS ${_mesh_o})

# Cross-compile the RV-side mesh host with one of the meshes baked in via CLI.
# `-march=rv64gcv` so pipeline_runtime.cpp uses the same vector ABI as the
# compiled shader — required because `__tex2d_sample` returns `<4 x float>`
# in v-registers when both sides agree on the V extension.
function(rv_mesh_target target_name spec_name spec_value)
    set(_bin ${RISCV_DIR}/${target_name}.rv)
    add_custom_command(
        OUTPUT  ${_bin}
        COMMAND ${CROSS_CXX_BIN} -std=c++20 -O3 -static -fopenmp
                -march=rv64gcv -mabi=lp64d
                -DANIM_NAME="${target_name}" -DNFRAMES=300 -DWIDTH=768 -DHEIGHT=768
                -DSHADER_PACKET_WIDTH=${_PKT_W}
                ${CMAKE_SOURCE_DIR}/test/rv_host/rv_host_mesh.cpp
                ${PIPELINE_DIR}/pipeline_runtime.cpp
                ${_mesh_o} -o ${_bin}
        DEPENDS ${_mesh_o} ${PIPELINE_DIR}/pipeline_runtime.cpp
                ${CMAKE_SOURCE_DIR}/test/rv_host/rv_host_mesh.cpp
        COMMENT "[rv]  ${target_name}.rv"
        VERBATIM)
    add_custom_target(${target_name}.rv DEPENDS ${_bin})
    if(QEMU_RISCV)
        # 300 frames @ 30fps = 10s animation, matching the GPU mesh targets
        # (vk-mesh* use nframes=300). Same length keeps cross-backend
        # comparison meaningful.
        add_custom_target(rv-${target_name}
            COMMAND ${CMAKE_COMMAND} -E env "OMP_NUM_THREADS=${NTHREADS}" "SHADER_PACKET=1"
                    ${RISCV_SIM} ${_bin} ${target_name} 300 ${spec_value}
            DEPENDS ${_bin}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            VERBATIM)
    endif()
endfunction()

if(CROSS_CXX_BIN)
    rv_mesh_target(mesh       sphere icosphere:3)
    rv_mesh_target(mesh_hi    sphere icosphere:5)
    rv_mesh_target(bunny      bunny  ${CMAKE_SOURCE_DIR}/test/assets/bunny.obj)
    rv_mesh_target(jeep       jeep   ${CMAKE_SOURCE_DIR}/test/assets/Jeep_Renegade_2016.obj)
    rv_mesh_target(teddy      teddy  ${CMAKE_SOURCE_DIR}/test/assets/teddy-bear/teddy-bear.obj)
    rv_mesh_target(boss       boss   "${CMAKE_SOURCE_DIR}/test/assets/boss/Rumba Dancing.obj")
endif()

# RISC-V terrain (linked VS + FS, OpenMP host driver).
set(_terrain_ll    ${RISCV_DIR}/terrain_rv.ll)
set(_terrain_vsll  ${RISCV_DIR}/terrain_vs_rv.ll)
set(_terrain_fsll  ${RISCV_DIR}/terrain_fs_rv.ll)
set(_terrain_gvnll ${RISCV_DIR}/terrain_rv.gvn.ll)
set(_terrain_optll ${RISCV_DIR}/terrain_rv.opt.ll)
set(_terrain_o     ${RISCV_DIR}/terrain_rv.o)
set(_terrain_rv    ${RISCV_DIR}/terrain.rv)

add_custom_command(
    OUTPUT  ${_terrain_ll}
    BYPRODUCTS ${_terrain_vsll} ${_terrain_fsll}
    COMMAND $<TARGET_FILE:irgen_riscv> ${_terrain_vsll} < ${ANIM_SHADER_DIR}/terrain_vs.src
    COMMAND $<TARGET_FILE:irgen_riscv> ${_terrain_fsll} < ${ANIM_SHADER_DIR}/terrain_fs.src
    COMMAND ${LLVM_LINK_BIN} ${_terrain_vsll} ${_terrain_fsll} ${TEX_INLINE_BC}
            -S -o ${_terrain_ll}
    DEPENDS ${ANIM_SHADER_DIR}/terrain_vs.src ${ANIM_SHADER_DIR}/terrain_fs.src
            irgen_riscv ${TEX_INLINE_BC}
    VERBATIM)
add_custom_target(terrain_rv.ll DEPENDS ${_terrain_ll})

add_custom_command(
    OUTPUT  ${_terrain_o}
    BYPRODUCTS ${_terrain_gvnll} ${_terrain_optll}
    COMMAND ${LLVM_OPT_BIN} -O3 --enable-unsafe-fp-math --fp-contract=fast -S
            ${_terrain_ll} -o ${_terrain_gvnll}
    COMMAND ${LLVM_OPT_BIN} -load-pass-plugin=${SINCOS_PLUGIN_PATH}
            -passes=sincos-opt,mem2reg,instcombine -S ${_terrain_gvnll} -o ${_terrain_optll}
    COMMAND ${LLVM_LLC_BIN} -O3 --fp-contract=fast -filetype=obj
            -relocation-model=pic -mtriple=riscv64-unknown-linux-gnu
            -mattr=+m,+a,+f,+d,+v ${_terrain_optll} -o ${_terrain_o}
    DEPENDS ${_terrain_ll} sincos_opt
    VERBATIM)
add_custom_target(terrain_rv.o DEPENDS ${_terrain_o})

if(CROSS_CXX_BIN)
    add_custom_command(
        OUTPUT  ${_terrain_rv}
        COMMAND ${CROSS_CXX_BIN} -std=c++20 -O3 -static -fopenmp
                -march=rv64gcv -mabi=lp64d
                -I${PIPELINE_DIR}
                -DANIM_NAME="terrain" -DNFRAMES=300 -DWIDTH=512 -DHEIGHT=512 -DVERT_COUNT=6144
                -DSHADER_PACKET_WIDTH=${_PKT_W}
                ${RV_HOST_SRC} ${PIPELINE_DIR}/pipeline_runtime.cpp
                ${_terrain_o} -o ${_terrain_rv}
        DEPENDS ${_terrain_o} ${PIPELINE_DIR}/pipeline_runtime.cpp ${RV_HOST_SRC}
        VERBATIM)
    add_custom_target(terrain.rv DEPENDS ${_terrain_rv})
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Phony aggregation targets (mirror the old Makefile)
# ─────────────────────────────────────────────────────────────────────────────
# `all-vk`: build every Vulkan animation .spv, render each via the host.
set(ALL_VK_DEPS spirv_vulkan_host quad.vert.spv)
foreach(anim ${ANIMATIONS_PROC})
    list(APPEND ALL_VK_DEPS ${anim}.frag.spv)
endforeach()
add_custom_target(all-vk
    DEPENDS ${ALL_VK_DEPS}
    COMMAND ${CMAKE_COMMAND} -E echo "Built all Vulkan animation shaders."
    VERBATIM)

if(CROSS_CXX_BIN)
    set(ALL_RV_DEPS)
    foreach(anim ${ANIMATIONS})
        list(APPEND ALL_RV_DEPS ${anim}.rv)
    endforeach()
    add_custom_target(all-rv
        DEPENDS ${ALL_RV_DEPS}
        COMMAND ${CMAKE_COMMAND} -E echo "Built all RISC-V animation binaries."
        VERBATIM)
endif()

# Top-level convenience target.
add_custom_target(tools ALL DEPENDS irgen_riscv irgen_spirv sincos_opt)

# Full reset: delegates to clean.sh (rm -rf build result). Works even though
# make is running inside build/ — rm happily unlinks the cwd's directory tree.
add_custom_target(distclean
    COMMAND bash ${CMAKE_SOURCE_DIR}/clean.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Removing build/ and result/ via clean.sh"
    VERBATIM)

# `cmake --build build --target clean` only removes the build artifacts
# tracked by CMake (objects, BYPRODUCTS, etc.). To fully reset, use:
#     rm -rf build result
# CMake's clean target architecturally cannot remove its own working dir or
# its own cache (deleting them would prevent the clean target from running).

# ─────────────────────────────────────────────────────────────────────────────
# Test / benchmark wrappers — delegate to the existing bash scripts.
# ─────────────────────────────────────────────────────────────────────────────
add_custom_target(check
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_tests.sh --no-build
    DEPENDS tools
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-fragment
    COMMAND ${CMAKE_COMMAND} -E env "SHADER_PACKET=1"
            bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_fragment.sh
    DEPENDS irgen_riscv spirv_vulkan_host quad.vert.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-fragment-quick
    COMMAND ${CMAKE_COMMAND} -E env "SHADER_PACKET=1"
            bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_fragment.sh --quick
    DEPENDS irgen_riscv spirv_vulkan_host quad.vert.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-compute
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_compute.sh --tiny
    DEPENDS spirv_vulkan_life_host life.comp.spv life_cs_rv.o
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-compute-blur
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_compute_blur.sh
    DEPENDS spirv_vulkan_compute_host blur.comp.spv blur_cs_rv.o
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-vertex
    COMMAND ${CMAKE_COMMAND} -E env "SHADER_PACKET=1"
            bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_vertex.sh
    DEPENDS spirv_vulkan_host terrain.vert.spv terrain.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-mesh
    COMMAND ${CMAKE_COMMAND} -E env "SHADER_PACKET=1"
            bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_mesh.sh
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-diverge
    COMMAND ${CMAKE_COMMAND} -E env "SHADER_PACKET=1"
            bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_diverge.sh
    DEPENDS spirv_vulkan_host irgen_riscv quad.vert.spv mandelbrot.frag.spv diverge.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(cpu-scaling
    COMMAND ${CMAKE_COMMAND} -E env "SHADER_PACKET=1"
            bash ${CMAKE_SOURCE_DIR}/test/script/run_cpu_scaling.sh
    DEPENDS irgen_riscv life_cs_rv.o blur_cs_rv.o
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

# ─────────────────────────────────────────────────────────────────────────────
# `compare-backends`: static, hardware-independent structural comparison of the
# SPIR-V and RISC-V backends (both descend from the same LLVM IR). Builds every
# .spv and _rv.o the script reads, then prints the per-shader table. No QEMU,
# hardware, or GPU involved — see test/script/compare_backends.sh.
# ─────────────────────────────────────────────────────────────────────────────
set(COMPARE_DEPS irgen_riscv irgen_spirv)
foreach(anim ${ANIMATIONS_PROC})
    list(APPEND COMPARE_DEPS ${anim}.frag.spv ${anim}_rv.o)
endforeach()
foreach(cs life blur)
    list(APPEND COMPARE_DEPS ${cs}.comp.spv ${cs}_cs_rv.o)
endforeach()
list(APPEND COMPARE_DEPS
     mesh.vert.spv mesh.frag.spv mesh_rv.o
     terrain.vert.spv terrain.frag.spv terrain_rv.o)
add_custom_target(compare-backends
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/compare_backends.sh
    DEPENDS ${COMPARE_DEPS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

# ─────────────────────────────────────────────────────────────────────────────
# spirv-val conformance over every emitted module (test/script/run_spirv_val.sh).
# The dependency list is collected by the spv_compile_* helpers into the global
# SPV_MODULES property, so a newly registered shader is validated automatically
# rather than needing to be remembered here.
# ─────────────────────────────────────────────────────────────────────────────
get_property(SPV_MODULES GLOBAL PROPERTY SPV_MODULES)
add_custom_target(spirv-validate
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_spirv_val.sh --no-build
    DEPENDS ${SPV_MODULES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)
