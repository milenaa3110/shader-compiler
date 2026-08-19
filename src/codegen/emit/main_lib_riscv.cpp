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
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <iostream>
#include <iterator>
#include <string>

#include "emit_trampolines.h"
#include "emit_fs_packet.h"

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
    if (hasStageEntry && !emitPipelineTrampolines()) {
        logError("Pipeline trampoline emission failed");
        return 1;
    }

    // Route B: emit a width-W SPMD `fs_packet` variant of the fragment shader
    // when it is within the packetizer's supported subset. Always attempted (the
    // runtime calls it via a weak symbol when SHADER_PACKET=1, else falls back to
    // the scalar fs_invoke); the scalar path is untouched whether or not this
    // fires. SHADER_EMIT_PACKET=verbose just makes the outcome chatty for tests.
    if (hasStageEntry) {
        fspacket::PacketEmitter pe;
        bool ok = pe.run(nodes);
        if (std::getenv("SHADER_EMIT_PACKET"))
            std::cout << (ok ? "Emitted packet (SPMD width "
                             : "packet: bailed (")
                      << (ok ? std::to_string(fspacket::kW) + ")" : "unsupported constructs)")
                      << "\n";
    }

    // Build-width marker: the runtime checks this against its PACKET_W. A width
    // mismatch would silently corrupt the SoA stride, so make it a loud failure.
    // LinkOnceODR so vs+fs modules llvm-link'd into one .rv object merge cleanly
    // (all built with the same kW → same value). It has no in-module users, so
    // append it to llvm.used — otherwise `opt -O3` (GlobalDCE) strips it before
    // llc and the runtime's strong reference fails to link.
    if (!TheModule->getGlobalVariable("__shader_packet_width")) {
        auto* i32Ty = Type::getInt32Ty(*Context);
        auto* marker = new GlobalVariable(*TheModule, i32Ty, /*isConstant=*/true,
                           GlobalValue::LinkOnceODRLinkage,
                           ConstantInt::get(i32Ty, fspacket::kW),
                           "__shader_packet_width");
        llvm::appendToUsed(*TheModule, {marker});
    }

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