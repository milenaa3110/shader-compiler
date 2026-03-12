// main_lib_riscv.cpp — irgen_riscv: same pipeline as irgen but stamps the
// module with the RISC-V target triple and RVV feature attributes so that
// llc-18 emits native RISC-V code with vector (RVV) auto-vectorisation.

#include "../parser/parser.h"
#include "../ast/ast.h"
#include "../codegen_state/codegen_state.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>

#include "emit_trampolines.h"

extern int CurTok;
int getNextToken();

using namespace llvm;

static void InitializeModule() {
    Context   = std::make_unique<llvm::LLVMContext>();
    TheModule = std::make_unique<llvm::Module>("shader_module", *Context);
    Builder   = std::make_unique<llvm::IRBuilder<>>(*Context);
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

    // Emit pipeline trampolines for stage-entry shaders
    bool hasStageEntry = false;
    for (auto& F : *TheModule)
        if (F.hasMetadata("shader.stage")) { hasStageEntry = true; break; }
    if (hasStageEntry)
        emitPipelineTrampolines();

    // ── Stamp RISC-V target triple + data layout ────────────────────────────
    TheModule->setTargetTriple("riscv64-unknown-linux-gnu");
    TheModule->setDataLayout("e-m:e-p:64:64-i64:64-i128:128-n32:64-S128");

    // Tag ALL non-declaration functions (including trampolines) with RVV attrs.
    for (auto& F : *TheModule) {
        if (F.isDeclaration()) continue;
        F.addFnAttr("target-cpu", "generic-rv64");
        F.addFnAttr("target-features", "+m,+a,+f,+d,+v,+zve64f");
    }

    if (llvm::verifyModule(*TheModule, &llvm::errs())) {
        std::cerr << "Invalid LLVM module.\n"; return 1;
    }

    std::error_code EC;
    llvm::raw_fd_ostream OS("module.ll", EC, llvm::sys::fs::OF_Text);
    if (EC) { std::cerr << "Cannot open module.ll: " << EC.message() << "\n"; return 1; }
    TheModule->print(OS, nullptr);
    std::cout << "Wrote module.ll (RISC-V + RVV target)\n";
    return 0;
}