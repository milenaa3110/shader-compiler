#ifndef AST_H
#define AST_H

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include "lexer.h"

// Forward-declare umesto #include <llvm/...>
namespace llvm { class Value; }

inline void printIndent(int indent) {
    for (int i = 0; i < indent; ++i)
        std::cout << "  ";
}

// ===============================
// Osnovna klasa za sve izraze
// ===============================
class ExprAST {
public:
    virtual ~ExprAST() = default;
    // codegen će biti definisan u .cpp (ast_codegen.cpp)
    virtual llvm::Value* codegen() = 0;
    virtual void print(int indent = 0) const = 0;
};

// ===============================
// Broj (npr. 3.14)
// ===============================
class NumberExprAST : public ExprAST {
public:
    double Val;

    explicit NumberExprAST(double val) : Val(val) {}
    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Number: " << Val << "\n";
    }
};

// ===============================
// Promenljiva (npr. a, b, result)
// ===============================
class VariableExprAST : public ExprAST {
public:
    std::string Name;

    explicit VariableExprAST(const std::string &name) : Name(name) {}
    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Variable: " << Name << "\n";
    }
};

// ===============================
// Binarni operator (npr. a + b)
// ===============================
class BinaryExprAST : public ExprAST {
public:
    int Op; // koristimo int jer u parseru čuvaš tokene (npr. tok_plus)
    std::unique_ptr<ExprAST> LHS, RHS;

    BinaryExprAST(int op,
                  std::unique_ptr<ExprAST> lhs,
                  std::unique_ptr<ExprAST> rhs)
        : Op(op), LHS(std::move(lhs)), RHS(std::move(rhs)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "BinaryOp: ";
        char BinOpChar = '?';
        if (Op == tok_plus || Op == '+') BinOpChar = '+';
        std::cout << BinOpChar << "\n";     // novi red da se ne lepi sa LHS
        LHS->print(indent + 1);
        RHS->print(indent + 1);
    }
};

// =============================================
// Poziv funkcije (npr. vec3(1.0, 2.0, 3.0))
// =============================================
class CallExprAST : public ExprAST {
public:
    std::string Callee;
    std::vector<std::unique_ptr<ExprAST>> Args;

    CallExprAST(const std::string &callee,
                std::vector<std::unique_ptr<ExprAST>> args)
        : Callee(callee), Args(std::move(args)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Call: " << Callee << "\n";
        for (const auto &arg : Args)
            arg->print(indent + 1);
    }
};

// ===============================
// Dodela (npr. vec3 a = ...)
// ===============================
class AssignmentExprAST : public ExprAST {
public:
    std::string VarName;
    std::string VarType; // npr. "vec3"
    std::unique_ptr<ExprAST> Init;   // <— preimenovano sa Value -> Init

    AssignmentExprAST(const std::string &type,
                      const std::string &name,
                      std::unique_ptr<ExprAST> init)
        : VarName(name), VarType(type), Init(std::move(init)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Assignment:\n";
        printIndent(indent + 1);
        std::cout << "Type: " << VarType << "\n";
        printIndent(indent + 1);
        std::cout << "Name: " << VarName << "\n";
        printIndent(indent + 1);
        std::cout << "Value:\n";
        Init->print(indent + 2);
    }
};
#endif // AST_H
