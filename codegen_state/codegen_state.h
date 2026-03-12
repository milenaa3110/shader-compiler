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

// Stage variable descriptor (in/out declarations at file scope)
struct StageVar {
    std::string name;
    std::string typeName;
    bool isInput; // true = "in", false = "out"
    int  binding; // -1 if no layout(binding=N)
};

// Resource binding descriptor (sampler/image uniforms)
struct ResourceBinding {
    std::string typeName; // "sampler2D", "samplerCube", "image2D", ...
    std::string name;
    int         binding;
};

extern std::unique_ptr<llvm::LLVMContext> Context;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;
extern std::map<std::string, llvm::AllocaInst*> NamedValues;
extern std::unordered_map<std::string, StructInfo> NamedStructTypes;
extern std::vector<llvm::BasicBlock*> BreakStack;
extern std::vector<llvm::BasicBlock*> ContinueStack;  // for 'continue' statement
extern std::map<std::string, llvm::GlobalVariable*> UniformArrays;

// Stage variable registries — populated by StageVarDeclAST::codegen()
extern std::vector<StageVar> StageInputVars;
extern std::vector<StageVar> StageOutputVars;
// Resource bindings for samplers/images
extern std::vector<ResourceBinding> ResourceBindings;

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
