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
QEMU_USER     ?= qemu-riscv64
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

# Binaries
TARGET_CODEGEN := shader_codegen
TARGET_IRGEN   := irgen

# Sources (shared)
COMMON_SRCS := \
  $(LEXER_DIR)/lexer.cpp \
  $(PARSER_DIR)/parser.cpp \
  $(CODEGEN_DIR)/codegen_state.cpp \
  $(AST_DIR)/ast.cpp \
  $(HELPERS_DIR)/call_helpers.cpp \
  $(HELPERS_DIR)/assignment_helpers.cpp

CODEGEN_SRCS := $(COMMON_SRCS) $(MAINCODEGEN_DIR)/main_codegen.cpp
IRGEN_SRCS   := $(COMMON_SRCS) $(MAIN_DIR)/main_lib.cpp

CODEGEN_OBJS := $(patsubst %.cpp,$(OBJDIR_LLVM)/%.o,$(notdir $(CODEGEN_SRCS)))
IRGEN_OBJS   := $(patsubst %.cpp,$(OBJDIR_LLVM)/irgen_%.o,$(notdir $(IRGEN_SRCS)))

# ---- default ----
.PHONY: all
all: $(TARGET_CODEGEN) $(TARGET_IRGEN)

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

# ---- unit tests ----
TEST_SCRIPT := test/run_tests.sh

.PHONY: check check-verbose
check: all
	@bash $(TEST_SCRIPT) --no-build

check-verbose: all
	@bash $(TEST_SCRIPT) --no-build --verbose

# ---- clean ----
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET_CODEGEN) $(TARGET_IRGEN) \
	      module.ll module.opt.ll shader.o shader_native.o \
	      librvshade.so test_host.rv render_host
	rm -rf $(RESULT_DIR)
