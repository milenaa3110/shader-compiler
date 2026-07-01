// main_lib_spirv.cpp — irgen_spirv

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
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "emit_spirv_from_ir.h"

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

    // Slurp the whole shader from stdin; the source string must outlive
    // ParseProgram (the lexer views into it).
    std::string source((std::istreambuf_iterator<char>(std::cin)),
                        std::istreambuf_iterator<char>());
    diag::setSource(source);  // enable caret diagnostics for parse/sema/codegen

    ASTContext astCtx;
    auto nodes = ParseProgram(astCtx, source);
    if (nodes.empty()) {
        logError("[irgen_spirv] Parse failed or empty program");
        return 1;
    }

    // Post-parse semantic pass — see main_lib_riscv.cpp for rationale.
    SemanticAnalyzer sema(astCtx);
    if (sema.run(nodes) != 0) {
        logError("[irgen_spirv] Semantic analysis failed");
        return 1;
    }

    // Forward-declare all structs so codegen handles out-of-order field
    // references — see main_lib_riscv.cpp.
    for (auto* n : nodes) {
        if (auto* sd = llvm::dyn_cast_or_null<StructDeclExprAST>(n))
            sd->predeclare();
    }

    for (auto* n : nodes) {
        if (n && !n->codegen()) {
            logError("[irgen_spirv] Codegen failed");
            return 1;
        }
    }

    if (llvm::verifyModule(*TheModule, &llvm::errs())) {
        logError("[irgen_spirv] Invalid LLVM module");
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

    if (getenv("IRGEN_SPIRV_DUMP_IR")) {
        TheModule->print(llvm::errs(), nullptr);
    }

    // Translate LLVM IR -> SPIR-V binary directly.
    std::vector<uint8_t> spv = emitSPIRVFromIR(*TheModule, stage);
    if (spv.empty()) {
        logError("[irgen_spirv] Emitter produced no output");
        return 1;
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        logErrorFmt("[irgen_spirv] Cannot write {}", outPath);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(spv.data()), spv.size());
    if (!out) {
        logErrorFmt("[irgen_spirv] Write failed for {}", outPath);
        return 1;
    }

    std::cout << "Wrote " << outPath << " (" << spv.size() << " bytes)\n";
    return 0;
}
