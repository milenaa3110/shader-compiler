# Toolchain (host)
CXX    := g++
CXXSTD := -std=c++20
CXXWARN:= -Wall -Wextra

# LLVM
# LLVM_CXXFLAGS := $(shell llvm-config --cxxflags 2>/dev/null)
# LLVM_LDFLAGS  := $(shell llvm-config --ldflags --system-libs --libs core support 2>/dev/null)
# LLVM_OPT      ?= opt
# LLC           ?= llc
# LLVM — auto-detect version (prefer 18, fall back to 17, then unversioned)
LLVM_CONFIG   := $(shell which llvm-config-18 2>/dev/null || which llvm-config-17 2>/dev/null || which llvm-config 2>/dev/null || echo llvm-config-18)
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags --system-libs --libs core support)
LLVM_OPT      := $(shell which opt-18 2>/dev/null || which opt-17 2>/dev/null || which opt 2>/dev/null || echo opt-18)
LLC           := $(shell which llc-18 2>/dev/null || which llc-17 2>/dev/null || which llc 2>/dev/null || echo llc-18)


# RISC-V toolchain
RISCV_TRIPLE  ?= riscv64-unknown-linux-gnu
RISCV_SYSROOT ?= /usr/riscv64-linux-gnu
NTHREADS      := $(shell nproc)
HOST_ARCH     := $(shell uname -m)

ifeq ($(HOST_ARCH),riscv64)
    # Native RISC-V hardware — run binaries directly, no cross-compiler needed
    CROSS_CXX ?= g++
    RISCV_SIM :=
    $(info [riscv] Native RISC-V host — running directly)
else
    # Cross-compiling from x86_64
    CROSS_CXX ?= riscv64-linux-gnu-g++
    QEMU_USER := $(shell which qemu-riscv64-static 2>/dev/null || which qemu-riscv64 2>/dev/null)
    ifneq ($(QEMU_USER),)
        RISCV_SIM := $(QEMU_USER) -L $(RISCV_SYSROOT)
        $(info [riscv] Using QEMU (OpenMP-capable): $(QEMU_USER))
    else
        RISCV_SIM := echo "ERROR: no RISC-V simulator found (install qemu-user-static)." &&
    endif
endif

# Dirs
LEXER_DIR      := lexer
PARSER_DIR     := parser
AST_DIR        := ast
CODEGEN_DIR    := codegen_state
HELPERS_DIR    := helpers
MAIN_DIR       := main
MAINCODEGEN_DIR:= main_codegen
TEST_DIR_RISCV := test/rv_host

BUILD_DIR      := build
SPV_DIR        := build/spirv
RISCV_DIR      := build/riscv
OBJDIR_LLVM    := $(BUILD_DIR)/llvm
PASSES_DIR     := passes
SINCOS_PLUGIN  := $(OBJDIR_LLVM)/sincos_opt.so

PIPELINE_DIR   := pipeline

# Binaries — compiler tools
TARGET_CODEGEN      := $(BUILD_DIR)/shader_codegen
TARGET_IRGEN_RISCV  := $(RISCV_DIR)/irgen_riscv
TARGET_IRGEN_SPIRV  := $(SPV_DIR)/irgen_spirv

# Binaries — Vulkan hosts
VK_HOST         := $(SPV_DIR)/spirv_vulkan_host
VK_COMPUTE_HOST := $(SPV_DIR)/spirv_vulkan_compute_host
VK_LIFE_HOST    := $(SPV_DIR)/spirv_vulkan_life_host
VK_TEXTURE_HOST := $(SPV_DIR)/spirv_vulkan_texture_host


# Sources (shared)
COMMON_SRCS := \
  $(LEXER_DIR)/lexer.cpp \
  $(PARSER_DIR)/parser.cpp \
  $(CODEGEN_DIR)/codegen_state.cpp \
  $(AST_DIR)/ast.cpp \
  $(HELPERS_DIR)/call_helpers.cpp \
  $(HELPERS_DIR)/assignment_helpers.cpp

CODEGEN_SRCS       := $(COMMON_SRCS) $(MAINCODEGEN_DIR)/main_codegen.cpp
IRGEN_RISCV_SRCS   := $(COMMON_SRCS) $(MAIN_DIR)/main_lib_riscv.cpp
IRGEN_SPIRV_SRCS   := $(COMMON_SRCS) $(MAIN_DIR)/main_lib_spirv.cpp

CODEGEN_OBJS       := $(patsubst %.cpp,$(OBJDIR_LLVM)/%.o,$(notdir $(CODEGEN_SRCS)))
IRGEN_RISCV_OBJS   := $(patsubst %.cpp,$(OBJDIR_LLVM)/irgen_riscv_%.o,$(notdir $(IRGEN_RISCV_SRCS)))
IRGEN_SPIRV_OBJS   := $(patsubst %.cpp,$(OBJDIR_LLVM)/irgen_spirv_%.o,$(notdir $(IRGEN_SPIRV_SRCS)))

# ---- default ----
.PHONY: all
all: $(TARGET_CODEGEN) $(TARGET_IRGEN_RISCV) $(TARGET_IRGEN_SPIRV) $(SINCOS_PLUGIN)

# ---- build dirs ----
$(OBJDIR_LLVM):
	@mkdir -p $(OBJDIR_LLVM)

$(RISCV_DIR):
	@mkdir -p $(RISCV_DIR)

$(SPV_DIR):
	@mkdir -p $(SPV_DIR)

# ---- compile rules ----
# CODEGEN objects
$(OBJDIR_LLVM)/%.o: $(LEXER_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/%.o: $(PARSER_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -I$(PARSER_DIR) -I$(AST_DIR) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/%.o: $(CODEGEN_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/%.o: $(AST_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(AST_DIR) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/%.o: $(HELPERS_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(AST_DIR) -I$(CODEGEN_DIR) -I$(HELPERS_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/%.o: $(MAINCODEGEN_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -I$(PARSER_DIR) -I$(AST_DIR) -I$(CODEGEN_DIR) -I$(HELPERS_DIR) -MMD -MP -fPIC -c $< -o $@

# IRGEN_RISCV objects (prefix irgen_riscv_)
$(OBJDIR_LLVM)/irgen_riscv_%.o: $(MAIN_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -I$(PARSER_DIR) -I$(AST_DIR) -I$(CODEGEN_DIR) -I$(HELPERS_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_riscv_%.o: $(LEXER_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_riscv_%.o: $(PARSER_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -I$(PARSER_DIR) -I$(AST_DIR) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_riscv_%.o: $(CODEGEN_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_riscv_%.o: $(AST_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(AST_DIR) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_riscv_%.o: $(HELPERS_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(AST_DIR) -I$(CODEGEN_DIR) -I$(HELPERS_DIR) -MMD -MP -fPIC -c $< -o $@

# IRGEN_SPIRV objects (prefix irgen_spirv_)
$(OBJDIR_LLVM)/irgen_spirv_%.o: $(MAIN_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -I$(PARSER_DIR) -I$(AST_DIR) -I$(CODEGEN_DIR) -I$(HELPERS_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_spirv_%.o: $(LEXER_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_spirv_%.o: $(PARSER_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -I$(PARSER_DIR) -I$(AST_DIR) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_spirv_%.o: $(CODEGEN_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_spirv_%.o: $(AST_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(AST_DIR) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_spirv_%.o: $(HELPERS_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(AST_DIR) -I$(CODEGEN_DIR) -I$(HELPERS_DIR) -MMD -MP -fPIC -c $< -o $@

# Auto-generated header dependencies
-include $(wildcard $(OBJDIR_LLVM)/*.d)

# ---- link ----
$(TARGET_CODEGEN): $(OBJDIR_LLVM) $(CODEGEN_OBJS)
	$(CXX) $(CODEGEN_OBJS) -o $@ $(LLVM_LDFLAGS) -lfmt

$(TARGET_IRGEN_RISCV): $(OBJDIR_LLVM) $(RISCV_DIR) $(IRGEN_RISCV_OBJS)
	$(CXX) $(IRGEN_RISCV_OBJS) -o $@ $(LLVM_LDFLAGS) -lfmt

$(TARGET_IRGEN_SPIRV): $(OBJDIR_LLVM) $(SPV_DIR) $(IRGEN_SPIRV_OBJS)
	$(CXX) $(IRGEN_SPIRV_OBJS) -o $@ $(LLVM_LDFLAGS) -lfmt

# ── LLVM pass plugins ────────────────────────────────────────────────────────
# sincos_opt: combines llvm.sin.f32(X)+llvm.cos.f32(X) pairs → sincosf(X,&s,&c)
$(SINCOS_PLUGIN): $(PASSES_DIR)/sincos_opt.cpp | $(OBJDIR_LLVM)
	$(CXX) -std=c++17 -fPIC -shared $(LLVM_CXXFLAGS) $< -o $@

RESULT_DIR     := result

# interactive codegen run
.PHONY: run-codegen
run-codegen: $(TARGET_CODEGEN)
	./$(TARGET_CODEGEN)

# NOTE: radi samo ako shader_codegen ispisuje cist IR na stdout
.PHONY: run-codegen-opt
run-codegen-opt: $(TARGET_CODEGEN)
	./$(TARGET_CODEGEN) | $(LLVM_OPT) -O3 -S
	

# ---- pipeline shader sources ----
PIPELINE_VS_SRC := test/shaders/pipeline/triangle_vs.src
PIPELINE_FS_SRC := test/shaders/pipeline/triangle_fs.src
SCENE_VS_SRC    := test/shaders/pipeline/scene_vs.src
SCENE_FS_SRC    := test/shaders/pipeline/scene_fs.src

# ---- RISC-V pipeline (requires riscv64-linux-gnu-g++ + qemu-riscv64) ----
# Install: sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu qemu-user-static
$(RISCV_DIR)/pipeline_riscv.ll: $(PIPELINE_VS_SRC) $(PIPELINE_FS_SRC) $(TARGET_IRGEN_RISCV)
	@mkdir -p $(RISCV_DIR)
	./$(TARGET_IRGEN_RISCV) $(RISCV_DIR)/vs_rv.ll < $(PIPELINE_VS_SRC)
	./$(TARGET_IRGEN_RISCV) $(RISCV_DIR)/fs_rv.ll < $(PIPELINE_FS_SRC)
	llvm-link-18 $(RISCV_DIR)/vs_rv.ll $(RISCV_DIR)/fs_rv.ll -S -o $@

$(RISCV_DIR)/pipeline_riscv.o: $(RISCV_DIR)/pipeline_riscv.ll
	$(LLC) -O3 -filetype=obj -relocation-model=pic \
	    -mtriple=$(RISCV_TRIPLE) -mattr=+m,+a,+f,+d,+v \
	    $< -o $@

$(RISCV_DIR)/pipeline_host.rv: $(RISCV_DIR)/pipeline_riscv.o $(PIPELINE_DIR)/pipeline_runtime.cpp $(PIPELINE_DIR)/pipeline_host.cpp
	$(CROSS_CXX) $(CXXSTD) -O3 -I$(PIPELINE_DIR) \
	    $(PIPELINE_DIR)/pipeline_host.cpp \
	    $(PIPELINE_DIR)/pipeline_runtime.cpp \
	    $(RISCV_DIR)/pipeline_riscv.o -o $@

.PHONY: pipeline-riscv
pipeline-riscv: $(RISCV_DIR)/pipeline_host.rv
	@mkdir -p $(RESULT_DIR)
	$(RISCV_SIM) ./$(RISCV_DIR)/pipeline_host.rv

# ---- RISC-V animation (multi-frame) ----
ANIM_VS_SRC := test/shaders/pipeline/anim_vs.src
ANIM_FS_SRC := test/shaders/pipeline/anim_fs.src

$(RISCV_DIR)/anim_riscv.ll: $(ANIM_VS_SRC) $(ANIM_FS_SRC) $(TARGET_IRGEN_RISCV)
	@mkdir -p $(RISCV_DIR)
	./$(TARGET_IRGEN_RISCV) $(RISCV_DIR)/anim_vs_rv.ll < $(ANIM_VS_SRC)
	./$(TARGET_IRGEN_RISCV) $(RISCV_DIR)/anim_fs_rv.ll < $(ANIM_FS_SRC)
	llvm-link-18 $(RISCV_DIR)/anim_vs_rv.ll $(RISCV_DIR)/anim_fs_rv.ll -S -o $@

$(RISCV_DIR)/anim_riscv.o: $(RISCV_DIR)/anim_riscv.ll
	$(LLC) -O3 -filetype=obj -relocation-model=pic \
	    -mtriple=$(RISCV_TRIPLE) -mattr=+m,+a,+f,+d,+v \
	    $< -o $@

$(RISCV_DIR)/anim_host.rv: $(RISCV_DIR)/anim_riscv.o $(PIPELINE_DIR)/pipeline_runtime.cpp $(PIPELINE_DIR)/anim_host.cpp
	$(CROSS_CXX) $(CXXSTD) -O3 -I$(PIPELINE_DIR) \
	    $(PIPELINE_DIR)/anim_host.cpp \
	    $(PIPELINE_DIR)/pipeline_runtime.cpp \
	    $(RISCV_DIR)/anim_riscv.o -o $@

.PHONY: anim-riscv
anim-riscv: $(RISCV_DIR)/anim_host.rv
	@mkdir -p $(RESULT_DIR)
	$(RISCV_SIM) ./$(RISCV_DIR)/anim_host.rv

# ---- Vulkan SPIR-V animation (renders via LavaPipe, encodes result/anim_spirv.mp4) ----
# Requires: sudo apt install libvulkan-dev glslang-tools
SPIRV_SHADER_DIR := test/shaders/spirv_vulkan
GLSLANG := glslangValidator

$(SPV_DIR)/anim.vert.spv: $(SPIRV_SHADER_DIR)/anim.vert
	@mkdir -p $(SPV_DIR)
	$(GLSLANG) -V $< -o $@

$(SPV_DIR)/anim.frag.spv: $(SPIRV_SHADER_DIR)/anim.frag
	@mkdir -p $(SPV_DIR)
	$(GLSLANG) -V $< -o $@

VK_CXXFLAGS := -O2 -Wno-missing-field-initializers

$(VK_HOST): test/vk_host/vk_host_fragment.cpp | $(OBJDIR_LLVM) $(SPV_DIR)
	$(CXX) $(CXXSTD) $(VK_CXXFLAGS) -o $@ $< -lvulkan

.PHONY: anim-spirv-vulkan
anim-spirv-vulkan: $(VK_HOST) $(SPV_DIR)/anim.vert.spv $(SPV_DIR)/anim.frag.spv
	@mkdir -p $(RESULT_DIR)
	$(VK_HOST) $(SPV_DIR)/anim.vert.spv $(SPV_DIR)/anim.frag.spv

ANIM_SHADER_DIR := test/shaders/animations

# ---- Vulkan quad VS — compiled from quad_vs.src via irgen_spirv ----
$(SPV_DIR)/quad.vert.spv: $(ANIM_SHADER_DIR)/quad_vs.src $(TARGET_IRGEN_SPIRV)
	@mkdir -p $(SPV_DIR)
	./$(TARGET_IRGEN_SPIRV) $@ < $(ANIM_SHADER_DIR)/quad_vs.src

# ── Macro: one Vulkan animation target ─────────────────────────────────────
# Usage: $(call VULKAN_ANIM,name)
#   Compiles $(ANIM_SHADER_DIR)/<name>_fs.src → module.spv via irgen_spirv.
#   Produces: result/<name>.frag.spv  +  .PHONY target  vk-<name>
define VULKAN_ANIM
$(SPV_DIR)/$(1).frag.spv: $(ANIM_SHADER_DIR)/$(1)_fs.src $(TARGET_IRGEN_SPIRV)
	@mkdir -p $(SPV_DIR)
	./$(TARGET_IRGEN_SPIRV) $$@ < $(ANIM_SHADER_DIR)/$(1)_fs.src

.PHONY: vk-$(1)
vk-$(1): $(VK_HOST) $(SPV_DIR)/quad.vert.spv $(SPV_DIR)/$(1).frag.spv
	@mkdir -p $(RESULT_DIR)
	$(VK_HOST) $(SPV_DIR)/quad.vert.spv $(SPV_DIR)/$(1).frag.spv $(1) 60 512 512
endef

# ── Macro: one RISC-V animation target ─────────────────────────────────────
# Usage: $(call RISCV_ANIM,name)
define RISCV_ANIM
$(RISCV_DIR)/$(1)_rv.ll: $(ANIM_SHADER_DIR)/$(1)_fs.src test/shaders/pipeline/scene_vs.src $(TARGET_IRGEN_RISCV)
	@mkdir -p $(RISCV_DIR)
	./$(TARGET_IRGEN_RISCV) $(RISCV_DIR)/$(1)_vs_rv.ll < test/shaders/pipeline/scene_vs.src
	./$(TARGET_IRGEN_RISCV) $(RISCV_DIR)/$(1)_fs_rv.ll < $(ANIM_SHADER_DIR)/$(1)_fs.src
	llvm-link-18 $(RISCV_DIR)/$(1)_vs_rv.ll $(RISCV_DIR)/$(1)_fs_rv.ll -S -o $$@

$(RISCV_DIR)/$(1)_rv.o: $(RISCV_DIR)/$(1)_rv.ll $(SINCOS_PLUGIN)
	$(LLVM_OPT) -O3 --enable-unsafe-fp-math --fp-contract=fast -S \
	    $$< -o $(RISCV_DIR)/$(1)_rv.gvn.ll
	$(LLVM_OPT) -load-pass-plugin=./$(SINCOS_PLUGIN) \
	    -passes='sincos-opt,mem2reg,instcombine' -S \
	    $(RISCV_DIR)/$(1)_rv.gvn.ll -o $(RISCV_DIR)/$(1)_rv.opt.ll
	$(LLC) -O3 --fp-contract=fast -filetype=obj -relocation-model=pic \
	    -mtriple=$(RISCV_TRIPLE) -mattr=+m,+a,+f,+d,+v $(RISCV_DIR)/$(1)_rv.opt.ll -o $$@

$(RISCV_DIR)/$(1).rv: $(RISCV_DIR)/$(1)_rv.o $(PIPELINE_DIR)/pipeline_runtime.cpp test/rv_host/rv_host_fragment.cpp
	$(CROSS_CXX) $(CXXSTD) -O3 -static -fopenmp -I$(PIPELINE_DIR) \
	    -DANIM_NAME='"$(1)"' -DNFRAMES=60 -DWIDTH=512 -DHEIGHT=512 \
	    test/rv_host/rv_host_fragment.cpp $(PIPELINE_DIR)/pipeline_runtime.cpp \
	    $$< -o $$@

.PHONY: rv-$(1)
rv-$(1): $(RISCV_DIR)/$(1).rv
	@mkdir -p $(RESULT_DIR)
	OMP_NUM_THREADS=$(NTHREADS) $(RISCV_SIM) ./$(RISCV_DIR)/$(1).rv
endef

# ── Animation lists ────────────────────────────────────────────────────────
# Procedural fragment shaders (no texture sampler) — compiled by irgen_spirv.
ANIMATIONS_PROC := mandelbrot julia voronoi waves tunnel ripple galaxy fire reaction cellular earth scene3d diverge

# All animations including texture_test — used for RISC-V and the full list.
ANIMATIONS := $(ANIMATIONS_PROC) texture_test

$(foreach A,$(ANIMATIONS_PROC),$(eval $(call VULKAN_ANIM,$(A))))
$(foreach A,$(ANIMATIONS),$(eval $(call RISCV_ANIM,$(A))))

.PHONY: all-vk all-rv
all-vk: $(VK_HOST) $(SPV_DIR)/quad.vert.spv \
        $(foreach A,$(ANIMATIONS_PROC),$(SPV_DIR)/$(A).frag.spv)
	@for A in $(ANIMATIONS_PROC); do \
	    echo "=== Vulkan: $$A ==="; \
	    $(VK_HOST) $(SPV_DIR)/quad.vert.spv $(SPV_DIR)/$$A.frag.spv $$A 60 512 512; \
	done

all-rv: $(foreach A,$(ANIMATIONS),$(RISCV_DIR)/$(A).rv)
	@for A in $(ANIMATIONS); do \
	    echo "=== RISC-V: $$A ($(NTHREADS) threads) ==="; \
	    OMP_NUM_THREADS=$(NTHREADS) $(RISCV_SIM) ./$(RISCV_DIR)/$$A.rv; \
	done

# ---- Vulkan Compute host ----
$(VK_COMPUTE_HOST): test/vk_host/vk_host_compute_blur.cpp | $(OBJDIR_LLVM) $(SPV_DIR)
	$(CXX) $(CXXSTD) $(VK_CXXFLAGS) -o $@ $< -lvulkan

$(SPV_DIR)/blur.comp.spv: $(ANIM_SHADER_DIR)/blur_cs.src $(TARGET_IRGEN_SPIRV)
	@mkdir -p $(SPV_DIR)
	./$(TARGET_IRGEN_SPIRV) $@ < $<

$(RISCV_DIR)/blur_cs_rv.ll: $(ANIM_SHADER_DIR)/blur_cs.src $(TARGET_IRGEN_RISCV)
	@mkdir -p $(RISCV_DIR)
	./$(TARGET_IRGEN_RISCV) $@ < $<

$(RISCV_DIR)/blur_cs_rv.o: $(RISCV_DIR)/blur_cs_rv.ll $(SINCOS_PLUGIN)
	$(LLVM_OPT) -O3 --enable-unsafe-fp-math --fp-contract=fast -S \
	    $< -o $(RISCV_DIR)/blur_cs_rv.gvn.ll
	$(LLVM_OPT) -load-pass-plugin=./$(SINCOS_PLUGIN) \
	    -passes='sincos-opt,mem2reg,instcombine' -S \
	    $(RISCV_DIR)/blur_cs_rv.gvn.ll -o $(RISCV_DIR)/blur_cs_rv.opt.ll
	$(LLC) -O3 --fp-contract=fast -filetype=obj -relocation-model=pic \
	    -mtriple=$(RISCV_TRIPLE) -mattr=+m,+a,+f,+d,+v \
	    $(RISCV_DIR)/blur_cs_rv.opt.ll -o $@

.PHONY: benchmark-compute-blur
benchmark-compute-blur: $(VK_COMPUTE_HOST) $(SPV_DIR)/blur.comp.spv $(RISCV_DIR)/blur_cs_rv.o
	@mkdir -p $(RESULT_DIR)
	@bash test/script/run_benchmark_compute_blur.sh

# ---- Vulkan Life host ----
$(VK_LIFE_HOST): test/vk_host/vk_host_compute.cpp | $(OBJDIR_LLVM) $(SPV_DIR)
	$(CXX) $(CXXSTD) $(VK_CXXFLAGS) -o $@ $< -lvulkan

$(SPV_DIR)/life.comp.spv: $(ANIM_SHADER_DIR)/life_cs.src $(TARGET_IRGEN_SPIRV)
	@mkdir -p $(SPV_DIR)
	./$(TARGET_IRGEN_SPIRV) $@ < $<

$(RISCV_DIR)/life_cs_rv.ll: $(ANIM_SHADER_DIR)/life_cs.src $(TARGET_IRGEN_RISCV)
	@mkdir -p $(RISCV_DIR)
	./$(TARGET_IRGEN_RISCV) $@ < $<

$(RISCV_DIR)/life_cs_rv.o: $(RISCV_DIR)/life_cs_rv.ll $(SINCOS_PLUGIN)
	$(LLVM_OPT) -O3 --enable-unsafe-fp-math --fp-contract=fast -S \
	    $< -o $(RISCV_DIR)/life_cs_rv.gvn.ll
	$(LLVM_OPT) -load-pass-plugin=./$(SINCOS_PLUGIN) \
	    -passes='sincos-opt,mem2reg,instcombine' -S \
	    $(RISCV_DIR)/life_cs_rv.gvn.ll -o $(RISCV_DIR)/life_cs_rv.opt.ll
	$(LLC) -O3 --fp-contract=fast -filetype=obj -relocation-model=pic \
	    -mtriple=$(RISCV_TRIPLE) -mattr=+m,+a,+f,+d,+v \
	    $(RISCV_DIR)/life_cs_rv.opt.ll -o $@

.PHONY: benchmark-compute
benchmark-compute: $(VK_LIFE_HOST) $(SPV_DIR)/life.comp.spv $(RISCV_DIR)/life_cs_rv.o
	@mkdir -p $(RESULT_DIR)
	@bash test/script/run_benchmark_compute.sh --tiny

.PHONY: benchmark-compute-sweep
benchmark-compute-sweep: $(VK_LIFE_HOST) $(SPV_DIR)/life.comp.spv $(RISCV_DIR)/life_cs_rv.o
	@mkdir -p $(RESULT_DIR)
	@bash test/script/run_benchmark_compute.sh --sweep

.PHONY: benchmark-compute-animate
benchmark-compute-animate: $(VK_LIFE_HOST) $(SPV_DIR)/life.comp.spv $(RISCV_DIR)/life_cs_rv.o
	@mkdir -p $(RESULT_DIR)
	@bash test/script/run_benchmark_compute.sh --animate

# ---- Vulkan Texture host ----
$(VK_TEXTURE_HOST): test/vk_host/vk_host_texture.cpp | $(OBJDIR_LLVM) $(SPV_DIR)
	$(CXX) $(CXXSTD) $(VK_CXXFLAGS) -o $@ $< -lvulkan

$(SPV_DIR)/texture_test.frag.spv: $(ANIM_SHADER_DIR)/texture_test_gpu_fs.src $(TARGET_IRGEN_SPIRV)
	@mkdir -p $(SPV_DIR)
	./$(TARGET_IRGEN_SPIRV) $@ < $(ANIM_SHADER_DIR)/texture_test_gpu_fs.src

.PHONY: vk-texture
vk-texture: $(VK_TEXTURE_HOST) $(SPV_DIR)/quad.vert.spv $(SPV_DIR)/texture_test.frag.spv
	@mkdir -p $(RESULT_DIR)
	$(VK_TEXTURE_HOST) $(SPV_DIR)/quad.vert.spv $(SPV_DIR)/texture_test.frag.spv texture_test 60 512 512

# ---- Terrain mesh animation (vertex shader drives a 32x32 grid, 6144 verts) ----
# Vulkan: compile terrain_vs_vk.src (negated Y) + terrain_fs.src → SPV
$(SPV_DIR)/terrain.vert.spv: $(ANIM_SHADER_DIR)/terrain_vs_vk.src $(TARGET_IRGEN_SPIRV)
	@mkdir -p $(SPV_DIR)
	./$(TARGET_IRGEN_SPIRV) $@ < $(ANIM_SHADER_DIR)/terrain_vs_vk.src

$(SPV_DIR)/terrain.frag.spv: $(ANIM_SHADER_DIR)/terrain_fs.src $(TARGET_IRGEN_SPIRV)
	@mkdir -p $(SPV_DIR)
	./$(TARGET_IRGEN_SPIRV) $@ < $(ANIM_SHADER_DIR)/terrain_fs.src

.PHONY: vk-terrain
vk-terrain: $(VK_HOST) $(SPV_DIR)/terrain.vert.spv $(SPV_DIR)/terrain.frag.spv
	@mkdir -p $(RESULT_DIR)
	$(VK_HOST) $(SPV_DIR)/terrain.vert.spv $(SPV_DIR)/terrain.frag.spv terrain 60 512 512 6144

.PHONY: benchmark-vertex
benchmark-vertex: $(VK_HOST) $(RISCV_DIR)/terrain.rv
	@bash test/script/run_benchmark_vertex.sh

# RISC-V: compile terrain_vs.src (standard Y) + terrain_fs.src → linked LL → .o → .rv
$(RISCV_DIR)/terrain_rv.ll: $(ANIM_SHADER_DIR)/terrain_vs.src $(ANIM_SHADER_DIR)/terrain_fs.src $(TARGET_IRGEN_RISCV)
	@mkdir -p $(RISCV_DIR)
	./$(TARGET_IRGEN_RISCV) $(RISCV_DIR)/terrain_vs_rv.ll < $(ANIM_SHADER_DIR)/terrain_vs.src
	./$(TARGET_IRGEN_RISCV) $(RISCV_DIR)/terrain_fs_rv.ll < $(ANIM_SHADER_DIR)/terrain_fs.src
	llvm-link-18 $(RISCV_DIR)/terrain_vs_rv.ll $(RISCV_DIR)/terrain_fs_rv.ll -S -o $@

$(RISCV_DIR)/terrain_rv.o: $(RISCV_DIR)/terrain_rv.ll $(SINCOS_PLUGIN)
	$(LLVM_OPT) -O3 --enable-unsafe-fp-math --fp-contract=fast -S \
	    $< -o $(RISCV_DIR)/terrain_rv.gvn.ll
	$(LLVM_OPT) -load-pass-plugin=./$(SINCOS_PLUGIN) \
	    -passes='sincos-opt,mem2reg,instcombine' -S \
	    $(RISCV_DIR)/terrain_rv.gvn.ll -o $(RISCV_DIR)/terrain_rv.opt.ll
	$(LLC) -O3 --fp-contract=fast -filetype=obj -relocation-model=pic \
	    -mtriple=$(RISCV_TRIPLE) -mattr=+m,+a,+f,+d,+v $(RISCV_DIR)/terrain_rv.opt.ll -o $@

$(RISCV_DIR)/terrain.rv: $(RISCV_DIR)/terrain_rv.o $(PIPELINE_DIR)/pipeline_runtime.cpp test/rv_host/rv_host_fragment.cpp
	$(CROSS_CXX) $(CXXSTD) -O3 -static -fopenmp -I$(PIPELINE_DIR) \
	    -DANIM_NAME='"terrain"' -DNFRAMES=60 -DWIDTH=512 -DHEIGHT=512 -DVERT_COUNT=6144 \
	    test/rv_host/rv_host_fragment.cpp $(PIPELINE_DIR)/pipeline_runtime.cpp \
	    $< -o $@

.PHONY: rv-terrain
rv-terrain: $(RISCV_DIR)/terrain.rv
	@mkdir -p $(RESULT_DIR)
	OMP_NUM_THREADS=$(NTHREADS) $(RISCV_SIM) ./$(RISCV_DIR)/terrain.rv

# ---- unit tests ----
TEST_SCRIPT := test/script/run_tests.sh

.PHONY: check check-verbose
check: all
	@bash $(TEST_SCRIPT) --no-build

check-verbose: all
	@bash $(TEST_SCRIPT) --no-build --verbose


.PHONY: cpu-scaling
cpu-scaling: $(TARGET_IRGEN_RISCV) $(RISCV_DIR)/life_cs_rv.o $(RISCV_DIR)/blur_cs_rv.o
	@bash test/script/run_cpu_scaling.sh

.PHONY: cpu-scaling-quick
cpu-scaling-quick: $(TARGET_IRGEN_RISCV) $(RISCV_DIR)/life_cs_rv.o $(RISCV_DIR)/blur_cs_rv.o
	@bash test/script/run_cpu_scaling.sh --quick

.PHONY: bench-rvv-width
bench-rvv-width: $(TARGET_IRGEN_RISCV) $(RISCV_DIR)/life_cs_rv.o $(RISCV_DIR)/blur_cs_rv.o
	@bash test/script/run_cpu_scaling.sh --rvv-only

.PHONY: benchmark-fragment
benchmark-fragment: $(TARGET_IRGEN_RISCV) $(VK_HOST)
	@bash test/script/run_benchmark_fragment.sh

.PHONY: benchmark-fragment-quick
benchmark-fragment-quick: $(TARGET_IRGEN_RISCV) $(VK_HOST)
	@bash test/script/run_benchmark_fragment.sh --quick

.PHONY: benchmark-diverge
benchmark-diverge: $(VK_HOST) $(TARGET_IRGEN_RISCV) $(SPV_DIR)/diverge.frag.spv $(SPV_DIR)/mandelbrot.frag.spv $(SPV_DIR)/quad.vert.spv
	@mkdir -p $(RESULT_DIR)
	@bash test/script/run_benchmark_diverge.sh

.PHONY: benchmark-diverge-quick
benchmark-diverge-quick: $(VK_HOST) $(TARGET_IRGEN_RISCV) $(SPV_DIR)/diverge.frag.spv $(SPV_DIR)/mandelbrot.frag.spv $(SPV_DIR)/quad.vert.spv
	@mkdir -p $(RESULT_DIR)
	@bash test/script/run_benchmark_diverge.sh --quick

# ---- clean ----
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(RESULT_DIR)

