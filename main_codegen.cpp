//
// Created by Milena on 11/7/2025.
//

#include "parser.h"       // tvoje parsiranje
#include "AST.h"
#include "codegen_state.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>

extern int CurTok;
int getNextToken();

static void InitializeModule() {
    TheContext = std::make_unique<llvm::LLVMContext>();
    TheModule  = std::make_unique<llvm::Module>("shader_module", *TheContext);
    Builder    = std::make_unique<llvm::IRBuilder<>>(*TheContext);
}

int main() {
    std::cout << "Shader Parser (codegen V1). Enter input, end with EOF (Ctrl+D):\n";

    InitializeModule();

    // Napravi `int main()` funkciju u IR-u
    llvm::FunctionType* MainTy =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(*TheContext), false);
    llvm::Function* MainFn =
        llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage, "main", TheModule.get());
    llvm::BasicBlock* Entry = llvm::BasicBlock::Create(*TheContext, "entry", MainFn);
    Builder->SetInsertPoint(Entry);

    // Očisti mapu promenljivih za ovu funkciju
    NamedValues.clear();

    // Start lexiranja
    getNextToken();

    // čitaj i generiši kod za niz dodela: vec3 a = 1 + 2; itd.
    while (CurTok != tok_eof) {
        if (auto stmt = ParseAssignment()) {
            if (!stmt->codegen()) {
                std::cerr << "Codegen failed for statement.\n";
                return 1;
            }
        } else {
            std::cerr << "Parse error.\n";
            return 1;
        }
    }

    // `return 0;`
    Builder->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*TheContext), 0));

    if (llvm::verifyFunction(*MainFn, &llvm::errs())) {
        std::cerr << "Invalid function generated.\n";
        return 1;
    }

    TheModule->print(llvm::outs(), nullptr);
    return 0;
}
