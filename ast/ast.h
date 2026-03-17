#ifndef AST_H
#define AST_H

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <optional>
#include <array>
#include <cstdint>
#include <map>
#include "../lexer/lexer.h"
#include <array>
#include <optional>

// ── Shader stage and function attributes ─────────────────────────────────────
enum class ShaderStage { Vertex, Fragment, Compute };

struct FunctionAttrs {
    bool isEntry = false;
    std::optional<ShaderStage> stage;
    std::optional<std::array<uint32_t, 3>> workgroupSize;
};

// Forward declaration of LLVM classes
namespace llvm { class Value; }

enum class Builtin {
    Position,
    FragCoord,
    FragDepth,
    VertexId,
    InstanceId,
    GlobalInvocationId,
    LocalInvocationId,
    WorkgroupId,
    NumWorkgroups,
    LocalInvocationIndex
};

// Helper function to print indentation
inline void printIndent(int indent) {
    for (int i = 0; i < indent; ++i)
        std::cout << "  ";
}

// Base class for all expression nodes
class ExprAST {
public:
    virtual ~ExprAST() = default;
    virtual llvm::Value* codegen() = 0;
    virtual void print(int indent = 0) const = 0;
};

// Number literal
class NumberExprAST : public ExprAST {
public:
    double Val;
    bool   isInt;  // true if lexed without decimal point (e.g. 3, not 3.0)

    explicit NumberExprAST(double val, bool isInt_ = false)
        : Val(val), isInt(isInt_) {}
    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Number: " << Val << "\n";
    }
};

// Variable reference
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

// Unary operator
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

// Binary operator
class BinaryExprAST : public ExprAST {
public:
    int Op;
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

// Ternary operator: cond ? then : else
class TernaryExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Cond, ThenExpr, ElseExpr;

    TernaryExprAST(std::unique_ptr<ExprAST> cond,
                   std::unique_ptr<ExprAST> then_,
                   std::unique_ptr<ExprAST> else_)
        : Cond(std::move(cond)), ThenExpr(std::move(then_)), ElseExpr(std::move(else_)) {}

    llvm::Value* codegen() override;
    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Ternary\n";
        Cond->print(indent + 1);
        ThenExpr->print(indent + 1);
        ElseExpr->print(indent + 1);
    }
};

// Boolean literal
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

// Member access or swizzle
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

// Member assignment
class MemberAssignmentExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Object;
    std::string Member;
    std::unique_ptr<ExprAST> Init;

    MemberAssignmentExprAST(std::unique_ptr<ExprAST> obj, std::string mem, 
                           std::unique_ptr<ExprAST> init)
        : Object(std::move(obj)), Member(std::move(mem)), Init(std::move(init)) {}

    llvm::Value* codegen() override;
    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "MemberAssignment: ." << Member << " = ...\n";
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

// Matrix element assignment: mat[i] = col_vec  or  mat[i][j] = scalar
class MatrixAssignmentExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Object;  // the matrix variable
    std::unique_ptr<ExprAST> Index;   // column index
    std::unique_ptr<ExprAST> Index2;  // row index (nullptr => whole-column assignment)
    std::unique_ptr<ExprAST> RHS;

    MatrixAssignmentExprAST(std::unique_ptr<ExprAST> obj,
                            std::unique_ptr<ExprAST> idx,
                            std::unique_ptr<ExprAST> idx2,
                            std::unique_ptr<ExprAST> rhs)
        : Object(std::move(obj)), Index(std::move(idx)),
          Index2(std::move(idx2)), RHS(std::move(rhs)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "MatrixAssignment: [" << (Index2 ? "][" : "") << "] = ...\n";
        Object->print(indent + 1);
        RHS->print(indent + 1);
    }
};

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

// Assignment statement
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

// Function prototype
class PrototypeAST : public ExprAST {
public:
    std::string Name;
    std::vector<std::pair<std::string, std::string>> Args; // {type, name}
    std::string RetType;

    PrototypeAST(std::string name,
                 std::vector<std::pair<std::string,std::string>> args,
                 std::string ret)
      : Name(std::move(name)), Args(std::move(args)), RetType(std::move(ret)) {}

    llvm::Value* codegen() override;
    void print(int indent=0) const override {
        printIndent(indent); std::cout << "Proto: " << RetType << " " << Name << "(...)\n";
    }
};

// Function definition
class FunctionAST : public ExprAST {
public:
    std::unique_ptr<PrototypeAST> Proto;
    std::unique_ptr<ExprAST> Body;
    FunctionAttrs Attrs;

    FunctionAST(std::unique_ptr<PrototypeAST> proto,
                std::unique_ptr<ExprAST> body,
                FunctionAttrs attrs = {})
      : Proto(std::move(proto)), Body(std::move(body)), Attrs(std::move(attrs)) {}

    llvm::Value* codegen() override;
    void print(int indent=0) const override {
        printIndent(indent);
        std::cout << "Function: " << Proto->Name;

        if (Attrs.stage.has_value()) {
            std::cout << " [stage=";
            switch (*Attrs.stage) {
                case ShaderStage::Vertex:   std::cout << "vertex"; break;
                case ShaderStage::Fragment: std::cout << "fragment"; break;
                case ShaderStage::Compute:  std::cout << "compute"; break;
            }
            std::cout << "]";
        }
        if (Attrs.isEntry) std::cout << " [entry]";
        std::cout << "\n";

        Body->print(indent+1);
    }
};


// Return statement
class ReturnStmtAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Expr;

    explicit ReturnStmtAST(std::unique_ptr<ExprAST> e)
      : Expr(std::move(e)) {}

    llvm::Value* codegen() override;
    void print(int indent=0) const override {
        printIndent(indent); std::cout << "Return\n";
        if (Expr) Expr->print(indent+1);
    }
};

// While statement

class WhileExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Condition;
    std::unique_ptr<ExprAST> Body;

    WhileExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> B)
        : Condition(std::move(Cond)), Body(std::move(B)) {}

    llvm::Value* codegen() override;
    void print(int indent=0) const override {
        printIndent(indent);
        std::cout << "While\n";
        if (Condition) {
            printIndent(indent + 2);
            std::cout << "Condition:\n";
            Condition->print(indent + 4);
        }
        if (Body) {
            printIndent(indent + 2);
            std::cout << "Body:\n";
            Body->print(indent + 4);
        }
    }
};

// FOR STATEMENT

class ForExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> Init;
    std::unique_ptr<ExprAST> Condition;
    std::unique_ptr<ExprAST> Increment;
    std::unique_ptr<ExprAST> Body;

    ForExprAST(std::unique_ptr<ExprAST> I, std::unique_ptr<ExprAST> C,
               std::unique_ptr<ExprAST> Inc, std::unique_ptr<ExprAST> B)
        : Init(std::move(I)), Condition(std::move(C)),
          Increment(std::move(Inc)), Body(std::move(B)) {}

    llvm::Value* codegen() override;
    void print(int indent=0) const override {
        printIndent(indent);
        std::cout << "For\n";

        if (Init) {
            printIndent(indent + 2);
            std::cout << "Init:\n";
            Init->print(indent + 4);
        }
        if (Condition) {
            printIndent(indent + 2);
            std::cout << "Condition:\n";
            Condition->print(indent + 4);
        }
        if (Increment) {
            printIndent(indent + 2);
            std::cout << "Increment:\n";
            Increment->print(indent + 4);
        }
        if (Body) {
            printIndent(indent + 2);
            std::cout << "Body:\n";
            Body->print(indent + 4);
        }
    }
};

// BREAK STATEMENT

class BreakStmtAST : public ExprAST {
public:
    BreakStmtAST() = default;
    llvm::Value* codegen() override;
    void print(int indent=0) const override {
        printIndent(indent);
        std::cout << "Break\n";
    }
};

// Continue statement
class ContinueStmtAST : public ExprAST {
public:
    ContinueStmtAST() = default;
    llvm::Value* codegen() override;
    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Continue\n";
    }
};

// Discard statement (fragment stage only)
class DiscardStmtAST : public ExprAST {
public:
    DiscardStmtAST() = default;
    llvm::Value* codegen() override;
    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "Discard\n";
    }
};

// STRUCT DECLARATION

class StructDeclExprAST : public ExprAST {
public:
    std::string Name;
    std::vector<std::pair<std::string,std::string>> Fields; // (type, name)

    StructDeclExprAST(const std::string &name,
                      std::vector<std::pair<std::string,std::string>> fields)
        : Name(name), Fields(std::move(fields)) {}
    llvm::Value* codegen() override;
    void print(int indent=0) const override {
        printIndent(indent + 2);
        std::cout << "StructDecl: " << Name << "\n";
        for (const auto& field : Fields) {
            printIndent(indent + 4);
            std::cout << "Field: " << field.first << " " << field.second << "\n";
        }
    }
};

// Stage variable declaration: in/out type name;
class StageVarDeclAST : public ExprAST {
public:
    bool isInput;   // true = "in", false = "out"
    std::string TypeName;
    std::string Name;
    int binding;    // -1 if no layout(binding=N)

    StageVarDeclAST(bool isInput_, std::string typeName, std::string name, int binding_ = -1)
        : isInput(isInput_), TypeName(std::move(typeName)), Name(std::move(name)), binding(binding_) {}

    llvm::Value* codegen() override;
    void print(int indent = 0) const override {
        printIndent(indent + 2);
        std::cout << (isInput ? "StageIn: " : "StageOut: ") << TypeName << " " << Name << "\n";
    }
};

// UNIFORM DECLARATION

class UniformDeclExprAST : public ExprAST {
public:
    std::string TypeName;
    std::string Name;
    UniformDeclExprAST(const std::string &ty, const std::string &n) : TypeName(ty), Name(n) {}
    llvm::Value* codegen() override;
    void print(int indent=0) const override {
        printIndent(indent + 2);
        std::cout << "UniformDecl: " << TypeName << " " << Name << "\n";
    }
};

// ARRAY INITIALIZATION
class ArrayInitExprAST : public ExprAST {
public:
    std::vector<std::unique_ptr<ExprAST>> Elements;

    explicit ArrayInitExprAST(std::vector<std::unique_ptr<ExprAST>> elements)
        : Elements(std::move(elements)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "ArrayInit: { ";
        for (size_t i = 0; i < Elements.size(); ++i) {
            Elements[i]->print(0);
            if (i < Elements.size() - 1) std::cout << ", ";
        }
        std::cout << " }\n";
    }
};

// ARRAY DECLARATION

class ArrayDeclExprAST : public ExprAST {
public:
    std::string ElementType;
    std::string Name;
    int Size;
    std::unique_ptr<ExprAST> Init;

    ArrayDeclExprAST(const std::string& type, const std::string& name,
                     int size, std::unique_ptr<ExprAST> init)
        : ElementType(type), Name(name), Size(size), Init(std::move(init)) {}

    llvm::Value* codegen() override;

    void print(int indent = 0) const override {
        printIndent(indent);
        std::cout << "ArrayDecl: " << ElementType << " " << Name
                  << "[" << Size << "]\n";
        if (Init) Init->print(indent + 1);
    }
};

// UNIFORM ARRAY DECLARATION

class UniformArrayDeclExprAST : public ExprAST {
public:
    std::string TypeName;
    std::string Name;
    int Size;

    UniformArrayDeclExprAST(const std::string &ty, const std::string &n, int size)
        : TypeName(ty), Name(n), Size(size) {}

    llvm::Value* codegen() override;

    void print(int indent=0) const override {
        printIndent(indent + 2);
        std::cout << "UniformArrayDecl: " << TypeName << " " << Name
                  << "[" << Size << "]\n";
    }
};

// STORAGE BUFFER DECLARATION (compute shaders)
// layout(std430, binding=N) readonly/writeonly buffer ElemType name[];
class StorageBufferDeclAST : public ExprAST {
public:
    std::string ElemType;  // e.g. "uint", "vec4"
    std::string Name;      // variable name visible in shader body
    bool isReadOnly;       // true = readonly, false = writeonly
    int  binding;          // binding index

    StorageBufferDeclAST(std::string elemType, std::string name, bool ro, int bind)
        : ElemType(std::move(elemType)), Name(std::move(name)),
          isReadOnly(ro), binding(bind) {}

    llvm::Value* codegen() override;
    void print(int indent = 0) const override {
        printIndent(indent + 2);
        std::cout << "StorageBuffer: "
                  << (isReadOnly ? "readonly" : "writeonly")
                  << " " << ElemType << " " << Name
                  << "[] (binding=" << binding << ")\n";
    }
};

#endif // AST_H
