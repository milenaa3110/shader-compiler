// main_lib_spirv.cpp — irgen_spirv: emits a SPIR-V binary via llvm-spirv-18.
//
// Usage:
//   ./irgen_spirv < shader.src      → module.ll  (SPIR-V triple)  + module.spv
//
// Requires:  sudo apt install llvm-spirv-18
//   (provides /usr/lib/llvm-18/bin/llvm-spirv or /usr/bin/llvm-spirv-18)

#include "../parser/parser.h"
#include "../ast/ast.h"
#include "../codegen_state/codegen_state.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>
#include <cstdlib>   // system()
#include <unistd.h>  // access(), X_OK

extern int CurTok;
int getNextToken();

using namespace llvm;

static void InitializeModule() {
    Context   = std::make_unique<llvm::LLVMContext>();
    TheModule = std::make_unique<llvm::Module>("shader_module", *Context);
    Builder   = std::make_unique<llvm::IRBuilder<>>(*Context);
}

// Try to find llvm-spirv on this system, returns path or "".
static std::string findLlvmSpirvTool() {
    // Ordered preference: versioned → unversioned
    static const char* candidates[] = {
        "/usr/lib/llvm-18/bin/llvm-spirv",
        "/usr/bin/llvm-spirv-18",
        "/usr/local/bin/llvm-spirv",
        "/usr/bin/llvm-spirv",
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) {
        if (::access(candidates[i], X_OK) == 0)
            return candidates[i];
    }
    return "";
}

int main() {
    InitializeModule();
    NamedValues.clear();

    getNextToken();
    auto nodes = ParseProgram();
    if (nodes.empty()) { std::cerr << "Parse failed or program is empty.\n"; return 1; }

    for (auto& n : nodes) {
        if (n && !n->codegen()) { std::cerr << "Codegen failed.\n"; return 1; }
    }

    // ── Stamp SPIR-V target triple ──────────────────────────────────────────
    // spirv64 maps to OpenCL/Vulkan SPIR-V 64-bit.  spirv32 also works.
    TheModule->setTargetTriple("spirv64-unknown-unknown");

    if (llvm::verifyModule(*TheModule, &llvm::errs())) {
        std::cerr << "Invalid LLVM module.\n"; return 1;
    }

    // Write LLVM IR with SPIR-V triple
    std::error_code EC;
    llvm::raw_fd_ostream OS("module.ll", EC, llvm::sys::fs::OF_Text);
    if (EC) { std::cerr << "Cannot open module.ll: " << EC.message() << "\n"; return 1; }
    TheModule->print(OS, nullptr);
    OS.flush();
    std::cout << "Wrote module.ll (SPIR-V triple)\n";

    // ── Translate to SPIR-V binary via llvm-spirv ───────────────────────────
    std::string spirvTool = findLlvmSpirvTool();
    if (spirvTool.empty()) {
        std::cerr << "\n[irgen_spirv] llvm-spirv not found.\n"
                  << "Install with:  sudo apt install llvm-spirv-18\n"
                  << "Then re-run:   " << spirvTool << " module.ll -o module.spv\n";
        return 1;
    }

    std::string cmd = spirvTool + " module.ll -o module.spv 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "[irgen_spirv] llvm-spirv translation failed (exit " << rc << ").\n";
        return 1;
    }
    std::cout << "Wrote module.spv\n";
    return 0;
}