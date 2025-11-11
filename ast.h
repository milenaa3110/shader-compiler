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

class UnaryExprAST : public ExprAST {
public:
    int Op;
    std::unique_ptr<ExprAST> Operand;

    UnaryExprAST(int op, std::unique_ptr<ExprAST> operand)
        : Op(op), Operand(std::move(operand)) {}

    llvm::Value* codegen() override;
    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "UnaryOp: " << (char)Op << "\n";
        Operand->print(indent + 1);
    }
};

// ===============================
// Binarni operator (npr. a + b)
// ===============================
class BinaryExprAST : public ExprAST {
public:
    int Op; // token vrednost (npr. tok_plus ili '+')
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
        std::cout << BinOpChar << "\n";
        LHS->print(indent + 1);
        RHS->print(indent + 1);
    }
};

class BooleanExprAST : public ExprAST {
public:
    bool Val;

    explicit BooleanExprAST(bool val) : Val(val) {}
    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Boolean: " << (Val ? "true" : "false") << "\n";
    }
};

class SwizzleExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Object;
    std::string Swizzle; // npr. "xyxx", "zyx"

    SwizzleExprAST(std::unique_ptr<ExprAST> object, const std::string& swizzle)
        : Object(std::move(object)), Swizzle(swizzle) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Swizzle: ." << Swizzle << "\n";
        Object->print(indent + 1);
    }
};

class SwizzleAssignmentExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Object;
    std::string Swizzle;
    std::unique_ptr<ExprAST> Init;

    SwizzleAssignmentExprAST(std::unique_ptr<ExprAST> object, const std::string& swizzle,
                            std::unique_ptr<ExprAST> value)
        : Object(std::move(object)), Swizzle(swizzle), Init(std::move(value)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "SwizzleAssignment: ." << Swizzle << " = \n";
        Object->print(indent + 1);
        Init->print(indent + 1);
    }
};


class MatrixAccessExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Object;
    std::unique_ptr<ExprAST> Index;
    std::unique_ptr<ExprAST> Index2;

    MatrixAccessExprAST(std::unique_ptr<ExprAST> object,
                        std::unique_ptr<ExprAST> index,
                        std::unique_ptr<ExprAST> index2 = nullptr)
        : Object(std::move(object)), Index(std::move(index)), Index2(std::move(index2)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "MatrixAccess: []\n";
        Object->print(indent + 1);
        Index->print(indent + 1);
        if (Index2) Index2->print(indent + 1);
    }
};

class MemberAccessExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Object;
    std::string Member;

    MemberAccessExprAST(std::unique_ptr<ExprAST> object, const std::string& member)
        : Object(std::move(object)), Member(member) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "MemberAccess: ." << Member << "\n";
        Object->print(indent + 1);
    }
};


class MatrixAssignmentExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Object;
    std::unique_ptr<ExprAST> Index;
    std::unique_ptr<ExprAST> Index2; // može biti nullptr
    std::unique_ptr<ExprAST> Init;   // RHS

    MatrixAssignmentExprAST(std::unique_ptr<ExprAST> obj,
                            std::unique_ptr<ExprAST> i,
                            std::unique_ptr<ExprAST> j,
                            std::unique_ptr<ExprAST> init)
        : Object(std::move(obj)),
          Index(std::move(i)),
          Index2(std::move(j)),
          Init(std::move(init)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "MatrixAccess: []\n";
        Object->print(indent + 1);
        Index->print(indent + 1);
        if (Index2) Index2->print(indent + 1);
    }
};

// =============================================
// Poziv funkcije / tip-konstruktora (npr. vec3(...))
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

class IfExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Condition;
    std::unique_ptr<ExprAST> ThenExpr;
    std::unique_ptr<ExprAST> ElseExpr; // može biti nullptr

    IfExprAST(std::unique_ptr<ExprAST> cond,
              std::unique_ptr<ExprAST> then,
              std::unique_ptr<ExprAST> elseExpr = nullptr)
        : Condition(std::move(cond)), ThenExpr(std::move(then)), ElseExpr(std::move(elseExpr)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "If:\n";
        printIndent(indent + 1);
        std::cout << "Condition:\n";
        Condition->print(indent + 2);
        printIndent(indent + 1);
        std::cout << "Then:\n";
        ThenExpr->print(indent + 2);
        if (ElseExpr) {
            printIndent(indent + 1);
            std::cout << "Else:\n";
            ElseExpr->print(indent + 2);
        }
    }
};

class BlockExprAST : public ExprAST {
public:
    std::vector<std::unique_ptr<ExprAST>> Statements;

    BlockExprAST(std::vector<std::unique_ptr<ExprAST>> stmts)
        : Statements(std::move(stmts)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Block:\n";
        for (const auto& stmt : Statements) {
            stmt->print(indent + 1);
        }
    }
};

// ===============================
// Dodela (npr. vec3 a = ...)
// ===============================
class AssignmentExprAST : public ExprAST {
public:
    std::string VarName;
    std::string VarType; // npr. "vec3"
    std::unique_ptr<ExprAST> Init;

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
