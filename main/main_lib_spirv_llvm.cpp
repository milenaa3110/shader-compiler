// main_lib_spirv_llvm.cpp — irgen_spirv_llvm: unified LLVM IR path to SPIR-V.
//
// Pipeline: .src → lexer/parser → AST → LLVM IR codegen → emit_glsl_from_ir → GLSL 450 → glslangValidator → .spv
//
// This uses the same AST codegen as irgen_riscv but instead of stamping RISC-V
// target attributes, it translates the IR back to GLSL 450 and compiles to SPIR-V.
// The benefit is that both backends share the exact same frontend + IR codegen,
// ensuring semantic equivalence.
//
// Usage:
//   ./irgen_spirv_llvm <output.spv> < shader_fs.src

#include "../parser/parser.h"
#include "../ast/ast.h"
#include "../codegen_state/codegen_state.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "emit_glsl_from_ir.h"

extern int CurTok;
int getNextToken();

using namespace llvm;

static void InitializeModule() {
    Context   = std::make_unique<llvm::LLVMContext>();
    TheModule = std::make_unique<llvm::Module>("shader_module", *Context);
    Builder   = std::make_unique<llvm::IRBuilder<>>(*Context);
}

int main(int argc, char* argv[]) {
    const char* outPath = (argc >= 2) ? argv[1] : "module.spv";

    InitializeModule();
    NamedValues.clear();

    getNextToken();
    auto nodes = ParseProgram();
    if (nodes.empty()) {
        std::cerr << "[irgen_spirv_llvm] Parse failed or empty program.\n";
        return 1;
    }

    // Run AST codegen — produces LLVM IR (same as RISC-V path)
    for (auto& n : nodes) {
        if (n && !n->codegen()) {
            std::cerr << "[irgen_spirv_llvm] Codegen failed.\n";
            return 1;
        }
    }

    if (llvm::verifyModule(*TheModule, &llvm::errs())) {
        std::cerr << "[irgen_spirv_llvm] Invalid LLVM module.\n";
        return 1;
    }

    // Detect shader stage from metadata
    ShaderStage stage = ShaderStage::Fragment;
    for (auto& F : *TheModule) {
        if (auto* md = F.getMetadata("shader.stage")) {
            if (auto* mds = dyn_cast<MDString>(md->getOperand(0))) {
                std::string s = mds->getString().str();
                if (s == "vertex")  stage = ShaderStage::Vertex;
                else if (s == "compute") stage = ShaderStage::Compute;
                break;
            }
        }
    }

    // Translate LLVM IR → GLSL 450
    std::string glslSrc = emitGLSLFromIR(*TheModule);

    if (getenv("IRGEN_SPIRV_LLVM_DEBUG")) {
        std::cerr << "=== Generated GLSL (from IR) ===\n" << glslSrc
                  << "=== End GLSL ===\n";
    }

    // Write GLSL to temp file
    const char* tmpPath;
    const char* stageFlag;
    if (stage == ShaderStage::Vertex) {
        tmpPath   = "/tmp/_irgen_spirv_llvm_tmp.vert";
        stageFlag = "vert";
    } else if (stage == ShaderStage::Compute) {
        tmpPath   = "/tmp/_irgen_spirv_llvm_tmp.comp";
        stageFlag = "comp";
    } else {
        tmpPath   = "/tmp/_irgen_spirv_llvm_tmp.frag";
        stageFlag = "frag";
    }

    {
        std::ofstream f(tmpPath);
        if (!f) {
            std::cerr << "[irgen_spirv_llvm] Cannot write temp file " << tmpPath << "\n";
            return 1;
        }
        f << glslSrc;
    }

    // Find glslangValidator
    static const char* candidates[] = {
        "/usr/bin/glslangValidator",
        "/usr/local/bin/glslangValidator",
        nullptr
    };
    const char* glslang = nullptr;
    for (int i = 0; candidates[i]; ++i) {
        if (::access(candidates[i], X_OK) == 0) { glslang = candidates[i]; break; }
    }
    if (!glslang) {
        ::unlink(tmpPath);
        std::cerr << "[irgen_spirv_llvm] glslangValidator not found.\n"
                  << "Install with:  sudo apt install glslang-tools\n";
        return 1;
    }

    // Compile GLSL → SPIR-V
    std::string cmd = std::string(glslang) +
        " -V --target-env vulkan1.0 -S " + stageFlag +
        " " + tmpPath + " -o " + outPath + " 2>&1";
    int rc = std::system(cmd.c_str());
    ::unlink(tmpPath);

    if (rc != 0) {
        std::cerr << "[irgen_spirv_llvm] glslangValidator failed (exit " << rc << ").\n";
        if (!getenv("IRGEN_SPIRV_LLVM_DEBUG")) {
            std::cerr << "Tip: set IRGEN_SPIRV_LLVM_DEBUG=1 to see the generated GLSL.\n";
        }
        return 1;
    }

    std::cout << "Wrote " << outPath << "\n";
    return 0;
}
