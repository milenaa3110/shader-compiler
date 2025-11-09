#include "AST.h"
#include <iostream>

#include "codegen_state.h"
#include <llvm/IR/Verifier.h>

using namespace llvm;

// === NumberExprAST ===
Value* NumberExprAST::codegen() {
    return ConstantFP::get(*TheContext, APFloat(Val));
}

// === VariableExprAST ===
// očekujemo da je promenljiva prethodno alocirana u funkciji (alloca)
Value* VariableExprAST::codegen() {
    auto it = NamedValues.find(Name);
    if (it == NamedValues.end()) {
        std::cerr << "Unknown variable: " << Name << "\n";
        return nullptr;
    }
    AllocaInst* A = it->second;
    return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
}

// === BinaryExprAST ===
// V1: podržavamo samo + (i to skalarno); lako ćeš proširiti kasnije
Value* BinaryExprAST::codegen() {
    Value* L = LHS->codegen();
    Value* R = RHS->codegen();
    if (!L || !R) return nullptr;

    // ako parser koristi tvoj tok_plus (-50) ili ASCII '+'
    if (Op == tok_plus || Op == '+')
        return Builder->CreateFAdd(L, R, "addtmp");

    std::cerr << "Unsupported binary op (V1 only '+')\n";
    return nullptr;
}

// === CallExprAST ===
// V1: privremeno odbijamo pozive (npr. vec3(...)) – dodaćemo u V2
Value* CallExprAST::codegen() {
    std::cerr << "CallExpr not supported in V1 (e.g. vec3(...)).\n";
    return nullptr;
}

// === AssignmentExprAST ===
// alocira (ako treba) i store-uje vrednost (V1: sve tretiramo kao double)
llvm::Value* AssignmentExprAST::codegen() {
    // ignorisemo VarType u V1 — sve je double
    Function* F = Builder->GetInsertBlock()->getParent();
    Type* Ty = Type::getDoubleTy(*TheContext);

    AllocaInst* Alloca = nullptr;
    auto it = NamedValues.find(VarName);
    if (it == NamedValues.end()) {
        Alloca = CreateEntryBlockAlloca(F, VarName, Ty);
        NamedValues[VarName] = Alloca;
    } else {
        Alloca = it->second;
    }

    // Generiši vrednost za dodelu
    llvm::Value* Val = this->Init->codegen();
    if (!Val) return nullptr;

    if (Val->getType() != Ty) {
        std::cerr << "Type mismatch: expected double in V1\n";
        return nullptr;
    }

    Builder->CreateStore(Val, Alloca);
    return Val;
}

