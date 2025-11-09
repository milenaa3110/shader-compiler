//
// Created by Milena on 11/7/2025.
//

#ifndef CODEGEN_STATE_H
#define CODEGEN_STATE_H

#include <map>
#include <memory>
#include <string>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;

// var -> alloca (adresu promenljive čuvamo kao alloca u funkciji)
extern std::map<std::string, llvm::AllocaInst*> NamedValues;

inline llvm::AllocaInst* CreateEntryBlockAlloca(llvm::Function* F,
                                                const std::string& name,
                                                llvm::Type* ty) {
    llvm::IRBuilder<> Tmp(&F->getEntryBlock(), F->getEntryBlock().begin());
    return Tmp.CreateAlloca(ty, nullptr, name);
}

#endif
