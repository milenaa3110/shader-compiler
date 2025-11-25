# ================== Toolchain (host) ==================
CXX       := g++
CXXSTD    := -std=c++17
CXXWARN   := -Wall -Wextra

# ================== LLVM flags ==================
LLVM_CXXFLAGS := $(shell llvm-config --cxxflags 2>/dev/null)
LLVM_LDFLAGS  := $(shell llvm-config --ldflags --system-libs --libs core support 2>/dev/null)

# ================== RISC-V toolchain ==================
CLANG        ?= clang
CLANGXX      ?= clang++
LLC          ?= llc
QEMU_SYS     ?= qemu-system-riscv64
QEMU_USER    ?= gen5
RISCV_TRIPLE ?= riscv64-unknown-linux-gnu
RISCV_SYSROOT?= /usr/riscv64-linux-gnu

# Optional: use LLD for linking RISC-V binaries
USE_LLD ?= 1
CROSS_CXX ?= riscv64-linux-gnu-g++

# ================== Build directories ==================
OBJDIR_PARSER := build/parser
OBJDIR_LLVM   := build/llvm

$(shell mkdir -p $(OBJDIR_PARSER) $(OBJDIR_LLVM) >/dev/null)

# ================== Parser-only target (bez LLVM) ==================
TARGET_PARSER := parser_test
PARSER_SRCS   := main.cpp lexer.cpp parser.cpp ast_dummy.cpp
PARSER_OBJS   := $(addprefix $(OBJDIR_PARSER)/,$(PARSER_SRCS:.cpp=.o))
PARSER_DEPS   := lexer.h parser.h ast.h

# ================== Codegen (LLVM) ==================
TARGET_CODEGEN := shader_codegen
CODEGEN_SRCS   := lexer.cpp parser.cpp codegen_state.cpp ast.cpp main_codegen.cpp
CODEGEN_OBJS   := $(addprefix $(OBJDIR_LLVM)/,$(CODEGEN_SRCS:.cpp=.o))

# ================== IR generator ==================
TARGET_IRGEN := irgen
IRGEN_SRCS   := lexer.cpp parser.cpp codegen_state.cpp ast.cpp main_lib.cpp
IRGEN_OBJS   := $(addprefix $(OBJDIR_LLVM)/,$(IRGEN_SRCS:.cpp=.o))

# ================== Default target ==================
.PHONY: all
ifeq ($(strip $(LLVM_CXXFLAGS)),)
  $(info llvm-config not found; building only parser_test)
  all: $(TARGET_PARSER)
else
  all: $(TARGET_CODEGEN)
endif

# ================== Parser-only build ==================
$(TARGET_PARSER): $(PARSER_OBJS)
	$(CXX) $(CXXSTD) $(CXXWARN) $^ -o $@

$(OBJDIR_PARSER)/%.o: %.cpp $(PARSER_DEPS)
	$(CXX) $(CXXSTD) $(CXXWARN) -c $< -o $@

# ================== LLVM builds ==================
$(OBJDIR_LLVM)/%.o: %.cpp
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -fPIC -c $< -o $@

$(TARGET_CODEGEN): $(CODEGEN_OBJS)
	$(CXX) $^ -o $@ $(LLVM_LDFLAGS)

$(TARGET_IRGEN): $(IRGEN_OBJS)
	$(CXX) $^ -o $@ $(LLVM_LDFLAGS)

# ================== Od .src do .ll ==================
module.ll: shader.src $(TARGET_IRGEN)
	./$(TARGET_IRGEN) < shader.src
	@echo "Wrote module.ll"

# ================== RISC-V: .ll -> .o (PIC) -> .so ==================
shader.o: module.ll
	$(LLC) -filetype=obj -relocation-model=pic -mtriple=riscv64-unknown-linux-gnu \
		-mattr=+d,+f -float-abi=hard -o $@ $<

# ================== RISC-V: .o -> .so ==================
librvshade.so: shader.o
	$(CROSS_CXX) -shared -fPIC shader.o -o $@

test_host.rv: test_host.cpp librvshade.so
	$(CROSS_CXX) -O2 test_host.cpp -L. -lrvshade -Wl,-rpath,'$$ORIGIN' -o $@

.PHONY: run-riscv
run-riscv: test_host.rv
	@echo "Running RISC-V user-mode..."
	$(QEMU_USER) -L $(RISCV_SYSROOT) ./test_host.rv

.PHONY: riscv-test
riscv-test: module.ll shader.o librvshade.so test_host.rv
	@echo "Running RISC-V shader test (user-mode)..."
	$(QEMU_USER) -L $(RISCV_SYSROOT) ./test_host.rv
# ================== Helpers ==================
.PHONY: clean run run-codegen ir
clean:
	rm -f $(TARGET_PARSER) $(TARGET_CODEGEN) $(TARGET_IRGEN) \
	      $(OBJDIR_PARSER)/*.o $(OBJDIR_LLVM)/*.o \
	      module.ll shader.o librvshade.so test_host.rv out.ppm

run: $(TARGET_PARSER)
	./$(TARGET_PARSER)

run-codegen: $(TARGET_CODEGEN)
	./$(TARGET_CODEGEN)

ir-riscv: riscv-test
