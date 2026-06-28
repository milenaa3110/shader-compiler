// main_lib_riscv.cpp — irgen_riscv: same pipeline as irgen but stamps the
// module with the RISC-V target triple and RVV feature attributes so that
// llc-18 emits native RISC-V code with vector (RVV) auto-vectorisation.

#include "../../frontend/parser/parser.h"
#include "../../frontend/ast/ast.h"
#include "../../frontend/ast/ast_context.h"
#include "../../frontend/sema/sema.h"
#include "../codegen_state/codegen_state.h"
#include "../../common/error_utils_fmt.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>
#include <iterator>
#include <string>

#include "emit_trampolines.h"

using namespace llvm;

static void InitializeModule() {
    Context   = std::make_unique<llvm::LLVMContext>();
    TheModule = std::make_unique<llvm::Module>("shader_module", *Context);
    Builder   = std::make_unique<llvm::IRBuilder<>>(*Context);
}

int main(int argc, char* argv[]) {
    const char* outPath = (argc >= 2) ? argv[1] : "module.ll";
    InitializeModule();
    NamedValues.clear();

    // Slurp the whole shader from stdin;
    std::string source((std::istreambuf_iterator<char>(std::cin)),
                        std::istreambuf_iterator<char>());
    diag::setSource(source);  // enable diagnostics for parse/sema/codegen

    // Per-compilation arena. Drop after codegen finishes.
    ASTContext astCtx;
    auto nodes = ParseProgram(astCtx, source);
    if (nodes.empty()) { logError("Parse failed or program is empty"); return 1; }

    // Post-parse semantic pass
    SemanticAnalyzer sema(astCtx);
    if (sema.run(nodes) != 0) { logError("Semantic analysis failed"); return 1; }

    // Forward-declare all structs so codegen can resolve out-of-order
    for (auto* n : nodes) {
        if (auto* sd = llvm::dyn_cast_or_null<StructDeclExprAST>(n))
            sd->predeclare();
    }

    for (auto* n : nodes) {
        if (n && !n->codegen()) { logError("Codegen failed"); return 1; }
    }

    // Emit pipeline trampolines for stage-entry shaders
    bool hasStageEntry = false;
    for (auto& F : *TheModule)
        if (F.hasMetadata("shader.stage")) { hasStageEntry = true; break; }
    if (hasStageEntry)
        emitPipelineTrampolines();

    // Stamp RISC-V target triple + data layout
    TheModule->setTargetTriple("riscv64-unknown-linux-gnu");
    TheModule->setDataLayout("e-m:e-p:64:64-i64:64-i128:128-n32:64-S128");

    // Tag ALL non-declaration functions (including trampolines) with RVV attrs.
    for (auto& F : *TheModule) {
        if (F.isDeclaration()) continue;
        F.addFnAttr("target-cpu", "generic-rv64");
        F.addFnAttr("target-features", "+m,+a,+f,+d,+v,+zve64f");
    }

    if (llvm::verifyModule(*TheModule, &llvm::errs())) {
        logError("Invalid LLVM module"); return 1;
    }

    std::error_code EC;
    llvm::raw_fd_ostream OS(outPath, EC, llvm::sys::fs::OF_Text);
    if (EC) { logErrorFmt("Cannot open {}: {}", outPath, EC.message()); return 1; }
    TheModule->print(OS, nullptr);
    std::cout << "Wrote " << outPath << " (RISC-V + RVV target)\n";
    return 0;
}