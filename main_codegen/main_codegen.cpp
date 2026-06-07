//
// Created by Milena on 11/7/2025.
//

#include "../parser/parser.h"
#include "../ast/ast.h"
#include "../ast/ast_context.h"
#include "../sema/sema.h"
#include "../codegen_state/codegen_state.h"
#include "../error_utils_fmt.h"  // for diag::setSource (caret diagnostics)

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>
#include <iterator>
#include <string>

static void InitializeModule() {
    Context = std::make_unique<llvm::LLVMContext>();
    TheModule  = std::make_unique<llvm::Module>("shader_module", *Context);
    Builder    = std::make_unique<llvm::IRBuilder<>>(*Context);
}

int main() {
    InitializeModule();

    llvm::FunctionType* MainTy =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(*Context), false);
    llvm::Function* MainFn =
        llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", TheModule.get());
    llvm::BasicBlock* Entry = llvm::BasicBlock::Create(*Context, "entry", MainFn);
    Builder->SetInsertPoint(Entry);

    NamedValues.clear();

    // Slurp the whole shader from stdin; the source string must outlive
    // ParseProgram (the lexer views into it).
    std::string source((std::istreambuf_iterator<char>(std::cin)),
                        std::istreambuf_iterator<char>());
    diag::setSource(source);  // enable caret diagnostics for parse/sema/codegen

    ASTContext astCtx;
    auto nodes = ParseProgram(astCtx, source);
    if (nodes.empty()) {
        logError("Parse failed or program is empty");
        return 1;
    }

    SemanticAnalyzer sema;
    if (sema.run(nodes) != 0) {
        logError("Semantic analysis failed");
        return 1;
    }

    // Forward-declare structs so out-of-order field references work.
    for (auto* n : nodes) {
        if (auto* sd = llvm::dyn_cast_or_null<StructDeclExprAST>(n))
            sd->predeclare();
    }

    for (auto* n : nodes) {
        if (!n) continue;
        if (!n->codegen()) {
            logError("Codegen failed for a node");
            return 1;
        }
    }

    Builder->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Context), 0));
    if (llvm::verifyFunction(*MainFn, &llvm::errs())) {
        logError("Invalid function generated");
        return 1;
    }

    TheModule->print(llvm::outs(), nullptr);
    return 0;
}
