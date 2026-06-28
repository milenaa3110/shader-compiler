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

struct StructInfo {
    llvm::StructType* Type;
    std::vector<std::string> FieldNames;
};

// Interstage shader I/O variable descriptor.
struct StageVar {
    std::string name;
    std::string typeName;
    bool isInput;
    int  location; // Slot index, or -1 if unassigned.
};

// Resource binding descriptor for samplers and images.
struct ResourceBinding {
    std::string typeName;
    std::string name;
    int         binding;
};

extern std::unique_ptr<llvm::LLVMContext> Context;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;
extern std::map<std::string, llvm::AllocaInst*> NamedValues;
extern std::unordered_map<std::string, StructInfo> NamedStructTypes;
extern std::vector<llvm::BasicBlock*> BreakStack;
extern std::vector<llvm::BasicBlock*> ContinueStack;
extern std::map<std::string, llvm::GlobalVariable*> UniformArrays;

// Storage buffer descriptor populated by StorageBufferDeclAST.
struct StorageBufferInfo {
    llvm::GlobalVariable* gv;
    llvm::Type*           elemTy;
    bool                  isReadOnly;
};
extern std::map<std::string, StorageBufferInfo> StorageBufferInfos;

// Parameter directions (0=in, 1=out, 2=inout) mapped by function name.
// Used to lower pass-by-reference arguments at call sites.
extern std::map<std::string, std::vector<uint8_t>> FunctionParamDirs;

inline llvm::AllocaInst* CreateEntryBlockAlloca(llvm::Function* F,
                                                const std::string& name,
                                                llvm::Type* ty) {
    llvm::IRBuilder<> Tmp(&F->getEntryBlock(), F->getEntryBlock().begin());
    return Tmp.CreateAlloca(ty, nullptr, name);
}

llvm::Value* splatIfScalarTo(llvm::IRBuilder<>& B, llvm::Value* v, llvm::Type* dstTy);
unsigned getVectorLengthOr1(llvm::Type* T);
llvm::Type* getFloatElemOrType(llvm::Type* T);
llvm::Value* dotFloatVector(llvm::IRBuilder<>& B, llvm::Value* a, llvm::Value* b);

llvm::Value* codegenBuiltin(llvm::IRBuilder<>& B,
                                  llvm::Module* M,
                                  const std::string& name,
                                  const std::vector<llvm::Value*>& args);


#endif