# ====== Toolchain ======
CXX := g++
CXXWARN := -Wall -Wextra
CXXSTD := -std=c++17

# ====== Parser-only target (bez LLVM) ======
TARGET_PARSER := parser_test
PARSER_SRCS   := main.cpp lexer.cpp parser.cpp ast_dummy.cpp
PARSER_OBJS   := $(PARSER_SRCS:.cpp=.o)
PARSER_DEPS   := lexer.h parser.h ast.h

# ====== Opcioni CODEGEN (sa LLVM) ======
CODEGEN_SRCS  := lexer.cpp parser.cpp codegen_state.cpp ast.cpp main_codegen.cpp
CODEGEN_OBJS  := $(CODEGEN_SRCS:.cpp=.o)
TARGET_CODEGEN := shader_codegen


LLVM_CXXFLAGS := $(shell llvm-config --cxxflags 2>/dev/null)
LLVM_LDFLAGS  := $(shell llvm-config --ldflags --system-libs --libs core support 2>/dev/null)

ifeq ($(strip $(LLVM_CXXFLAGS)),)
  $(info llvm-config not found; skipping codegen build)
  CODEGEN_SRCS :=
  CODEGEN_OBJS :=
  TARGET_CODEGEN :=
endif

# ====== Default target ======
.PHONY: all
ifeq ($(strip $(CODEGEN_SRCS)),)
all: $(TARGET_PARSER)
else
all: $(TARGET_CODEGEN)
endif

# ====== Parser-only build ======
$(TARGET_PARSER): $(PARSER_OBJS)
	$(CXX) $(CXXSTD) $(CXXWARN) $^ -o $@

# Parser object files (bez LLVM flagova)
main.o lexer.o parser.o ast_dummy.o: %.o: %.cpp $(PARSER_DEPS)
	$(CXX) $(CXXSTD) $(CXXWARN) -c $< -o $@

# ====== Codegen (LLVM) build ======
$(CODEGEN_OBJS): %.o: %.cpp
	$(CXX) $(CXXSTD) $(CXXWARN) $(LLVM_CXXFLAGS) -c $< -o $@

$(TARGET_CODEGEN): $(CODEGEN_OBJS)
	$(CXX) $^ -o $@ $(LLVM_LDFLAGS)

# ====== Utility ======
.PHONY: clean run run-codegen
clean:
	rm -f $(TARGET_PARSER) $(TARGET_CODEGEN) *.o

run: $(TARGET_PARSER)
	./$(TARGET_PARSER)

run-codegen: $(TARGET_CODEGEN)
	./$(TARGET_CODEGEN)
