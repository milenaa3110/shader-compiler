# Toolchain (host)
CXX    := g++
CXXSTD := -std=c++20
CXXWARN:= -Wall -Wextra

# LLVM
# LLVM_CXXFLAGS := $(shell llvm-config --cxxflags 2>/dev/null)
# LLVM_LDFLAGS  := $(shell llvm-config --ldflags --system-libs --libs core support 2>/dev/null)
# LLVM_OPT      ?= opt
# LLC           ?= llc
# LLVM
LLVM_CONFIG   := llvm-config-18
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags --system-libs --libs core support)
LLVM_OPT      := opt-18
LLC           := llc-18


# RISC-V toolchain
QEMU_USER     ?= qemu-riscv64-static
RISCV_TRIPLE  ?= riscv64-unknown-linux-gnu
RISCV_SYSROOT ?= /usr/riscv64-linux-gnu
CROSS_CXX     ?= riscv64-linux-gnu-g++

# Dirs
LEXER_DIR      := lexer
PARSER_DIR     := parser
AST_DIR        := ast
CODEGEN_DIR    := codegen_state
HELPERS_DIR    := helpers
MAIN_DIR       := main
MAINCODEGEN_DIR:= main_codegen
INPUT_DIR      := input
TEST_DIR_RISCV := test/riscv

BUILD_DIR      := build
OBJDIR_LLVM    := $(BUILD_DIR)/llvm

PIPELINE_DIR   := pipeline

# Binaries
TARGET_CODEGEN      := shader_codegen
TARGET_IRGEN        := irgen
TARGET_IRGEN_RISCV  := irgen_riscv
TARGET_IRGEN_SPIRV  := irgen_spirv
TARGET_PIPELINE     := pipeline_host

# Sources (shared)
COMMON_SRCS := \
  $(LEXER_DIR)/lexer.cpp \
  $(PARSER_DIR)/parser.cpp \
  $(CODEGEN_DIR)/codegen_state.cpp \
  $(AST_DIR)/ast.cpp \
  $(HELPERS_DIR)/call_helpers.cpp \
  $(HELPERS_DIR)/assignment_helpers.cpp

CODEGEN_SRCS       := $(COMMON_SRCS) $(MAINCODEGEN_DIR)/main_codegen.cpp
IRGEN_SRCS         := $(COMMON_SRCS) $(MAIN_DIR)/main_lib.cpp
IRGEN_RISCV_SRCS   := $(COMMON_SRCS) $(MAIN_DIR)/main_lib_riscv.cpp
IRGEN_SPIRV_SRCS   := $(COMMON_SRCS) $(MAIN_DIR)/main_lib_spirv.cpp

CODEGEN_OBJS       := $(patsubst %.cpp,$(OBJDIR_LLVM)/%.o,$(notdir $(CODEGEN_SRCS)))
IRGEN_OBJS         := $(patsubst %.cpp,$(OBJDIR_LLVM)/irgen_%.o,$(notdir $(IRGEN_SRCS)))
IRGEN_RISCV_OBJS   := $(patsubst %.cpp,$(OBJDIR_LLVM)/irgen_riscv_%.o,$(notdir $(IRGEN_RISCV_SRCS)))
IRGEN_SPIRV_OBJS   := $(patsubst %.cpp,$(OBJDIR_LLVM)/irgen_spirv_%.o,$(notdir $(IRGEN_SPIRV_SRCS)))

# ---- default ----
.PHONY: all
all: $(TARGET_CODEGEN) $(TARGET_IRGEN) $(TARGET_IRGEN_RISCV) $(TARGET_IRGEN_SPIRV)

# ---- build dir ----
$(OBJDIR_LLVM):
	@mkdir -p $(OBJDIR_LLVM)

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

# IRGEN objects (prefix irgen_)
$(OBJDIR_LLVM)/irgen_%.o: $(MAIN_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -I$(PARSER_DIR) -I$(AST_DIR) -I$(CODEGEN_DIR) -I$(HELPERS_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_%.o: $(LEXER_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_%.o: $(PARSER_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(LEXER_DIR) -I$(PARSER_DIR) -I$(AST_DIR) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_%.o: $(CODEGEN_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_%.o: $(AST_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(AST_DIR) -I$(CODEGEN_DIR) -MMD -MP -fPIC -c $< -o $@
$(OBJDIR_LLVM)/irgen_%.o: $(HELPERS_DIR)/%.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -I$(AST_DIR) -I$(CODEGEN_DIR) -I$(HELPERS_DIR) -MMD -MP -fPIC -c $< -o $@

# Auto-generated header dependencies
-include $(wildcard $(OBJDIR_LLVM)/*.d)

# ---- link ----
$(TARGET_CODEGEN): $(OBJDIR_LLVM) $(CODEGEN_OBJS)
	$(CXX) $(CODEGEN_OBJS) -o $@ $(LLVM_LDFLAGS) -lfmt

$(TARGET_IRGEN): $(OBJDIR_LLVM) $(IRGEN_OBJS)
	$(CXX) $(IRGEN_OBJS) -o $@ $(LLVM_LDFLAGS) -lfmt

$(TARGET_IRGEN_RISCV): $(OBJDIR_LLVM) $(IRGEN_RISCV_OBJS)
	$(CXX) $(IRGEN_RISCV_OBJS) -o $@ $(LLVM_LDFLAGS) -lfmt

$(TARGET_IRGEN_SPIRV): $(OBJDIR_LLVM) $(IRGEN_SPIRV_OBJS)
	$(CXX) $(IRGEN_SPIRV_OBJS) -o $@ $(LLVM_LDFLAGS) -lfmt

# ---- IR pipeline ----
module.ll: $(INPUT_DIR)/shader.src $(TARGET_IRGEN)
	./$(TARGET_IRGEN) < $<

module.opt.ll: module.ll
	$(LLVM_OPT) -O3 -S module.ll -o module.opt.ll

# ---- RISC-V build from OPTIMIZED IR ----
shader.o: module.opt.ll
	$(LLC) -O3 -filetype=obj -relocation-model=pic -mtriple=$(RISCV_TRIPLE) \
		-mattr=+d,+f -float-abi=hard -o $@ $<

librvshade.so: shader.o
	$(CROSS_CXX) -shared -fPIC shader.o -o $@

test_host.rv: $(TEST_DIR_RISCV)/test_host.cpp librvshade.so
	$(CROSS_CXX) -O3 -flto $< -L. -lrvshade -Wl,-rpath,'$$ORIGIN' -o $@

RESULT_DIR     := result
TEST_HOST_SRC  := $(TEST_DIR_RISCV)/test_host.cpp

# ---- native render (no QEMU, host architecture) ----
shader_native.o: module.opt.ll
	llc-18 -O3 -filetype=obj -o $@ $<

render_host: $(TEST_HOST_SRC) shader_native.o
	$(CXX) $(CXXSTD) -O3 $(TEST_HOST_SRC) shader_native.o -o $@

.PHONY: render
render: module.opt.ll render_host
	@mkdir -p $(RESULT_DIR)
	./render_host
	@echo "Image: $(RESULT_DIR)/shader_out.ppm"

# ---- RISC-V render (requires QEMU) ----
.PHONY: run-riscv
run-riscv: test_host.rv
	@mkdir -p $(RESULT_DIR)
	$(QEMU_USER) -L $(RISCV_SYSROOT) ./test_host.rv

# interactive codegen run
.PHONY: run-codegen
run-codegen: $(TARGET_CODEGEN)
	./$(TARGET_CODEGEN)

# NOTE: radi samo ako shader_codegen ispisuje cist IR na stdout
.PHONY: run-codegen-opt
run-codegen-opt: $(TARGET_CODEGEN)
	./$(TARGET_CODEGEN) | $(LLVM_OPT) -O3 -S
	
# ---- convenience ----
.PHONY: ir opt so test
ir: module.ll
opt: module.opt.ll
so: librvshade.so
test: run-riscv

# ---- pipeline render (x86 host, VS+FS linked together) ----
PIPELINE_VS_SRC := test/shaders/pipeline/triangle_vs.src
PIPELINE_FS_SRC := test/shaders/pipeline/triangle_fs.src
SCENE_VS_SRC    := test/shaders/pipeline/scene_vs.src
SCENE_FS_SRC    := test/shaders/pipeline/scene_fs.src

vs.ll: $(PIPELINE_VS_SRC) $(TARGET_IRGEN)
	./$(TARGET_IRGEN) < $< && mv module.ll $@

fs.ll: $(PIPELINE_FS_SRC) $(TARGET_IRGEN)
	./$(TARGET_IRGEN) < $< && mv module.ll $@

scene_vs.ll: $(SCENE_VS_SRC) $(TARGET_IRGEN)
	./$(TARGET_IRGEN) < $< && mv module.ll $@

scene_fs.ll: $(SCENE_FS_SRC) $(TARGET_IRGEN)
	./$(TARGET_IRGEN) < $< && mv module.ll $@

pipeline.ll: vs.ll fs.ll
	llvm-link-18 vs.ll fs.ll -S -o $@

scene_pipeline.ll: scene_vs.ll scene_fs.ll
	llvm-link-18 scene_vs.ll scene_fs.ll -S -o $@

$(OBJDIR_LLVM)/pipeline_runtime.o: $(PIPELINE_DIR)/pipeline_runtime.cpp | $(OBJDIR_LLVM)
	$(CXX) $(CXXSTD) -O3 -fPIC -I$(PIPELINE_DIR) -c $< -o $@

pipeline_shader.o: pipeline.ll
	$(LLC) -O3 -filetype=obj -relocation-model=pic $< -o $@

$(TARGET_PIPELINE): pipeline_shader.o $(OBJDIR_LLVM)/pipeline_runtime.o $(PIPELINE_DIR)/pipeline_host.cpp
	$(CXX) $(CXXSTD) -O3 -I$(PIPELINE_DIR) \
	    $(PIPELINE_DIR)/pipeline_host.cpp pipeline_shader.o \
	    $(OBJDIR_LLVM)/pipeline_runtime.o -o $@

.PHONY: pipeline
pipeline: $(TARGET_PIPELINE)
	@mkdir -p $(RESULT_DIR)
	./$(TARGET_PIPELINE)

scene_pipeline_shader.o: scene_pipeline.ll
	$(LLC) -O3 -filetype=obj -relocation-model=pic $< -o $@

scene_pipeline_host: scene_pipeline_shader.o $(OBJDIR_LLVM)/pipeline_runtime.o $(PIPELINE_DIR)/pipeline_host.cpp
	$(CXX) $(CXXSTD) -O3 -I$(PIPELINE_DIR) \
	    $(PIPELINE_DIR)/pipeline_host.cpp scene_pipeline_shader.o \
	    $(OBJDIR_LLVM)/pipeline_runtime.o -o $@ \
	    -DPIPELINE_VERT_COUNT=6 -DPIPELINE_OUTPUT_PPM='"result/scene_pipeline_out.ppm"'

# ---- RISC-V pipeline (requires riscv64-linux-gnu-g++ + qemu-riscv64) ----
# Install: sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu qemu-user-static
pipeline_riscv.ll: $(PIPELINE_VS_SRC) $(PIPELINE_FS_SRC) $(TARGET_IRGEN_RISCV)
	./$(TARGET_IRGEN_RISCV) < $(PIPELINE_VS_SRC) && mv module.ll vs_rv.ll
	./$(TARGET_IRGEN_RISCV) < $(PIPELINE_FS_SRC) && mv module.ll fs_rv.ll
	llvm-link-18 vs_rv.ll fs_rv.ll -S -o $@

pipeline_riscv.o: pipeline_riscv.ll
	$(LLC) -O3 -filetype=obj -relocation-model=pic \
	    -mtriple=$(RISCV_TRIPLE) -mattr=+m,+a,+f,+d,+v \
	    $< -o $@

pipeline_host.rv: pipeline_riscv.o $(PIPELINE_DIR)/pipeline_runtime.cpp $(PIPELINE_DIR)/pipeline_host.cpp
	$(CROSS_CXX) $(CXXSTD) -O3 -I$(PIPELINE_DIR) \
	    $(PIPELINE_DIR)/pipeline_host.cpp \
	    $(PIPELINE_DIR)/pipeline_runtime.cpp \
	    pipeline_riscv.o -o $@

.PHONY: pipeline-riscv
pipeline-riscv: pipeline_host.rv
	@mkdir -p $(RESULT_DIR)
	$(QEMU_USER) -L $(RISCV_SYSROOT) ./pipeline_host.rv

# ---- RISC-V animation (multi-frame) ----
ANIM_VS_SRC := test/shaders/pipeline/anim_vs.src
ANIM_FS_SRC := test/shaders/pipeline/anim_fs.src

anim_riscv.ll: $(ANIM_VS_SRC) $(ANIM_FS_SRC) $(TARGET_IRGEN_RISCV)
	./$(TARGET_IRGEN_RISCV) < $(ANIM_VS_SRC) && mv module.ll anim_vs_rv.ll
	./$(TARGET_IRGEN_RISCV) < $(ANIM_FS_SRC) && mv module.ll anim_fs_rv.ll
	llvm-link-18 anim_vs_rv.ll anim_fs_rv.ll -S -o $@

anim_riscv.o: anim_riscv.ll
	$(LLC) -O3 -filetype=obj -relocation-model=pic \
	    -mtriple=$(RISCV_TRIPLE) -mattr=+m,+a,+f,+d,+v \
	    $< -o $@

anim_host.rv: anim_riscv.o $(PIPELINE_DIR)/pipeline_runtime.cpp test/riscv/anim_host.cpp
	$(CROSS_CXX) $(CXXSTD) -O3 -I$(PIPELINE_DIR) \
	    test/riscv/anim_host.cpp \
	    $(PIPELINE_DIR)/pipeline_runtime.cpp \
	    anim_riscv.o -o $@

.PHONY: anim-riscv
anim-riscv: anim_host.rv
	@mkdir -p $(RESULT_DIR)
	$(QEMU_USER) -L $(RISCV_SYSROOT) ./anim_host.rv

# ---- x86 animation (multi-frame, 60 fps, encodes result/anim_x86.mp4) ----
anim_x86.ll: $(ANIM_VS_SRC) $(ANIM_FS_SRC) $(TARGET_IRGEN)
	./$(TARGET_IRGEN) < $(ANIM_VS_SRC) && mv module.ll anim_vs_x86.ll
	./$(TARGET_IRGEN) < $(ANIM_FS_SRC) && mv module.ll anim_fs_x86.ll
	llvm-link-18 anim_vs_x86.ll anim_fs_x86.ll -S -o $@

anim_x86.o: anim_x86.ll
	$(LLC) -O3 -filetype=obj -relocation-model=pic $< -o $@

anim_host_x86: anim_x86.o $(PIPELINE_DIR)/pipeline_runtime.cpp $(PIPELINE_DIR)/anim_host.cpp
	$(CXX) $(CXXSTD) -O3 -I$(PIPELINE_DIR) \
	    $(PIPELINE_DIR)/anim_host.cpp \
	    $(PIPELINE_DIR)/pipeline_runtime.cpp \
	    anim_x86.o -o $@

.PHONY: anim-x86
anim-x86: anim_host_x86
	@mkdir -p $(RESULT_DIR)
	./anim_host_x86

# ---- SPIR-V animation (multi-frame IR + .spv per frame) ----
.PHONY: anim-spirv
anim-spirv: $(TARGET_IRGEN_SPIRV)
	@bash test/run_spirv_anim.sh

# ---- Vulkan SPIR-V animation (renders via LavaPipe, encodes result/anim_spirv.mp4) ----
# Requires: sudo apt install libvulkan-dev glslang-tools
SPIRV_SHADER_DIR := test/shaders/spirv_vulkan
GLSLANG := glslangValidator

result/anim.vert.spv: $(SPIRV_SHADER_DIR)/anim.vert
	@mkdir -p result
	$(GLSLANG) -V $< -o $@

result/anim.frag.spv: $(SPIRV_SHADER_DIR)/anim.frag
	@mkdir -p result
	$(GLSLANG) -V $< -o $@

spirv_vulkan_host: $(PIPELINE_DIR)/spirv_vulkan_host.cpp
	$(CXX) $(CXXSTD) -O2 -o $@ $< -lvulkan

.PHONY: anim-spirv-vulkan
anim-spirv-vulkan: spirv_vulkan_host result/anim.vert.spv result/anim.frag.spv
	@mkdir -p $(RESULT_DIR)
	./spirv_vulkan_host result/anim.vert.spv result/anim.frag.spv

# ---- unit tests ----
TEST_SCRIPT := test/run_tests.sh

.PHONY: check check-verbose
check: all
	@bash $(TEST_SCRIPT) --no-build

check-verbose: all
	@bash $(TEST_SCRIPT) --no-build --verbose

.PHONY: check-pipeline
check-pipeline: all
	@bash test/run_pipeline_tests.sh

# ---- clean ----
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET_CODEGEN) $(TARGET_IRGEN) $(TARGET_IRGEN_RISCV) $(TARGET_IRGEN_SPIRV) \
	      $(TARGET_PIPELINE) pipeline_host.rv anim_host.rv anim_host_x86 spirv_vulkan_host scene_pipeline_host \
	      module.ll module.opt.ll module.spv \
	      vs.ll fs.ll pipeline.ll scene_vs.ll scene_fs.ll scene_pipeline.ll \
	      vs_rv.ll fs_rv.ll pipeline_riscv.ll anim_vs_rv.ll anim_fs_rv.ll anim_riscv.ll \
	      shader.o shader_native.o pipeline_shader.o scene_pipeline_shader.o \
	      pipeline_riscv.o anim_riscv.o \
	      librvshade.so test_host.rv render_host
	rm -rf $(RESULT_DIR)
