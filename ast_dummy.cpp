//
// Created by Milena on 11/7/2025.
//

#include "AST.h"
#include <iostream>

// Dummy implementacije za parser-only build (bez LLVM)
llvm::Value* NumberExprAST::codegen() {
    std::cout << "NumberExpr: " << Val << std::endl;
    return nullptr;
}

llvm::Value* VariableExprAST::codegen() {
    std::cout << "VariableExpr: " << Name << std::endl;
    return nullptr;
}

llvm::Value* BinaryExprAST::codegen() {
    std::cout << "BinaryExpr: ";
    LHS->codegen();
    std::cout << " " << (char)Op << " ";
    RHS->codegen();
    std::cout << std::endl;
    return nullptr;
}

llvm::Value* CallExprAST::codegen() {
    std::cout << "CallExpr: " << Callee << std::endl;
    return nullptr;
}

llvm::Value* AssignmentExprAST::codegen() {
    std::cout << "Assignment: " << VarName << " = ";
    Init->codegen();
    return nullptr;
}