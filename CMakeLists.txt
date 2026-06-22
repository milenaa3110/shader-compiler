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
add_executable(shader_codegen src/driver/main_codegen.cpp)
target_link_libraries(shader_codegen PRIVATE compiler_common)
target_include_directories(shader_codegen PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(shader_codegen PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(irgen_riscv src/codegen/emit/main_lib_riscv.cpp)
target_link_libraries(irgen_riscv PRIVATE compiler_common)
target_include_directories(irgen_riscv PRIVATE ${CMAKE_SOURCE_DIR}/src)
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
                    -I${PIPELINE_DIR}
                    -DANIM_NAME="${name}" -DNFRAMES=300 -DWIDTH=512 -DHEIGHT=512
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
                    reaction cellular earth scene3d diverge city ocean)
set(ANIMATIONS ${ANIMATIONS_PROC} texture_test)

# Vulkan vertex shader for the full-screen quad.
spv_compile_vertex(quad ${ANIM_SHADER_DIR}/quad_vs.src)

# Per-animation Vulkan fragment + RISC-V object/binary.
foreach(anim ${ANIMATIONS_PROC})
    spv_compile_fragment(${anim} ${ANIM_SHADER_DIR}/${anim}_fs.src)
    rv_compile_animation(${anim})

    # Render targets matching the old Makefile's vk-<name> / rv-<name>.
    add_custom_target(vk-${anim}
        COMMAND $<TARGET_FILE:spirv_vulkan_host>
                ${SPV_DIR}/quad.vert.spv ${SPV_DIR}/${anim}.frag.spv
                ${anim} 300 512 512
        DEPENDS spirv_vulkan_host quad.vert.spv ${anim}.frag.spv
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM)
    if(CROSS_CXX_BIN)
        add_custom_target(rv-${anim}
            COMMAND ${CMAKE_COMMAND} -E env "OMP_NUM_THREADS=${NTHREADS}"
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
    COMMAND $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            mesh 300 icosphere:3
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
add_custom_target(vk-mesh-hi
    COMMAND $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            mesh_hi 300 icosphere:5
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
add_custom_target(vk-mesh-bunny
    COMMAND $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            bunny 300 ${CMAKE_SOURCE_DIR}/test/assets/bunny.obj
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
add_custom_target(vk-mesh-jeep
    COMMAND $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            jeep 300 ${CMAKE_SOURCE_DIR}/test/assets/Jeep_Renegade_2016.obj
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
add_custom_target(vk-mesh-teddy
    COMMAND $<TARGET_FILE:spirv_vulkan_mesh_host>
            ${SPV_DIR}/mesh.vert.spv ${SPV_DIR}/mesh.frag.spv
            teddy 300 ${CMAKE_SOURCE_DIR}/test/assets/teddy-bear/teddy-bear.obj
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
# OBJ filename has a space ("Rumba Dancing.obj") — quote at expansion time so
# the shell gets one argument. obj_loader resolves map_Kd relative to the OBJ
# dir, so the texture is picked up automatically.
add_custom_target(vk-mesh-boss
    COMMAND $<TARGET_FILE:spirv_vulkan_mesh_host>
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
            COMMAND ${CMAKE_COMMAND} -E env "OMP_NUM_THREADS=${NTHREADS}"
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
                -I${PIPELINE_DIR}
                -DANIM_NAME="terrain" -DNFRAMES=300 -DWIDTH=512 -DHEIGHT=512 -DVERT_COUNT=6144
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
add_custom_target(tools ALL DEPENDS shader_codegen irgen_riscv irgen_spirv sincos_opt)

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
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_fragment.sh
    DEPENDS irgen_riscv spirv_vulkan_host quad.vert.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-fragment-quick
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_fragment.sh --quick
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
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_vertex.sh
    DEPENDS spirv_vulkan_host terrain.vert.spv terrain.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-mesh
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_mesh.sh
    DEPENDS spirv_vulkan_mesh_host mesh.vert.spv mesh.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(benchmark-diverge
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_benchmark_diverge.sh
    DEPENDS spirv_vulkan_host irgen_riscv quad.vert.spv mandelbrot.frag.spv diverge.frag.spv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(cpu-scaling
    COMMAND bash ${CMAKE_SOURCE_DIR}/test/script/run_cpu_scaling.sh
    DEPENDS irgen_riscv life_cs_rv.o blur_cs_rv.o
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)
