//
// Created by Milena on 11/7/2025.
//

#include "../parser/parser.h"
#include "../ast/ast.h"
#include "../codegen_state/codegen_state.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>

extern int CurTok;
int getNextToken();

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

    getNextToken();

    auto nodes = ParseProgram();
    if (nodes.empty()) {
        std::cerr << "Parse failed or program is empty.\n";
        return 1;
    }

    for (auto &n : nodes) {
        if (!n) continue;
        if (!n->codegen()) {
            std::cerr << "Codegen failed for a node.\n";
            return 1;
        }
    }

    Builder->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Context), 0));
    if (llvm::verifyFunction(*MainFn, &llvm::errs())) {
        std::cerr << "Invalid function generated.\n";
        return 1;
    }

    TheModule->print(llvm::outs(), nullptr);
    return 0;
}
