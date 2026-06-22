#ifndef AST_H
#define AST_H

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <llvm/Support/Casting.h>

#include "../lexer/lexer.h"
#include "type.h"

// ── AST ownership model ─────────────────────────────────────────────────────
//
// AST nodes are allocated out of an `ASTContext` (see ast_context.h):
// a per-compilation BumpPtrAllocator + StringSaver + destructor list. Every
// `ExprAST*` you see below is *non-owning* — the context owns the storage
// and runs destructors when it's destroyed. As a result:
//
//   * Constructors take `ExprAST*`, never `std::unique_ptr<ExprAST>`.
//   * Child fields are raw `ExprAST*`. They're nullable; check before deref.
//   * `std::move()` and `.get()` from the unique_ptr era are gone — pass
//     the pointer through directly.
//
// `std::string`/`std::vector` members are still per-node owners of their
// own backing storage; the context's destruction walk calls each node's
// virtual destructor, which cleans those up before the bump pool resets.

// ── Shader stage and function attributes ─────────────────────────────────────
enum class ShaderStage { Vertex, Fragment, Compute };

struct FunctionAttrs {
  bool isEntry = false;
  std::optional<ShaderStage> stage;
  std::optional<std::array<uint32_t, 3>> workgroupSize;
};

// Forward declaration of LLVM classes
namespace llvm {
class Value;
}

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
  for (int i = 0; i < indent; ++i) std::cout << "  ";
}

// Base class for all expression nodes.
//
// Uses LLVM-style RTTI: every subclass passes its `Kind` to the base
// constructor and implements `static bool classof(const ExprAST*)`. Use
// `llvm::isa<T>(p)`, `llvm::dyn_cast<T>(p)`, `llvm::cast<T>(p)` — *not*
// C++ `dynamic_cast`. The `Kind` field is a single byte that fully
// determines the dynamic type; the inheritance chain is never walked.
class ExprAST {
 public:
  enum class Kind {
    Number,
    Variable,
    Unary,
    Binary,
    Ternary,
    Boolean,
    MemberAccess,
    MemberAssignment,
    MatrixAccess,
    MatrixAssignment,
    Call,
    If,
    Block,
    Assignment,
    Prototype,
    Function,
    Return,
    While,
    For,
    Break,
    Continue,
    Discard,
    StructDecl,
    StageVarDecl,
    UniformDecl,
    ArrayInit,
    ArrayDecl,
    UniformArrayDecl,
    StorageBufferDecl,
    PostfixIncr,
    ImplicitCast,
  };

  virtual ~ExprAST() = default;
  virtual llvm::Value* codegen() = 0;
  virtual void print(int indent = 0) const = 0;

  Kind getKind() const { return kind_; }

  // Source position of the construct's first token, stamped by the parser via
  // Parser::at(). A default (invalid) location is the "unstamped" signal — a
  // codegen error there prints "line 0, col 0", flagging a parse path that
  // missed the stamp.
  SourceLocation loc;

  // Semantic type of this expression, filled in by SemanticAnalyzer (Step 2 of
  // the type-system bring-up). nullptr until typed — during the bring-up only
  // some node kinds are typed, so callers must null-check. Once codegen
  // dispatches on it (Step 4) every value-producing node will carry one.
  const glsl::Type* getType() const { return Ty; }
  void setType(const glsl::Type* t) { Ty = t; }

 protected:
  // No default constructor: every subclass MUST forward its Kind. A missed
  // subclass becomes a compile error, not a silent classof() bug.
  explicit ExprAST(Kind k) : kind_(k) {}

 private:
  Kind kind_;
  const glsl::Type* Ty = nullptr;  // set by SemanticAnalyzer; see get/setType
};

// Number literal
class NumberExprAST : public ExprAST {
 public:
  double Val;
  bool isInt;       // true if without decimal point (e.g. 3, not 3.0)
  bool isUnsigned;  // true if the literal had a 'u' suffix (only when isInt)

  explicit NumberExprAST(double val, bool isInt_ = false,
                         bool isUnsigned_ = false)
      : ExprAST(Kind::Number),
        Val(val),
        isInt(isInt_),
        isUnsigned(isUnsigned_) {}
  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Number: " << Val << "\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Number;
  }
};

// Variable reference
class VariableExprAST : public ExprAST {
 public:
  std::string Name;

  explicit VariableExprAST(const std::string& name)
      : ExprAST(Kind::Variable), Name(name) {}
  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Variable: " << Name << "\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Variable;
  }
};

// Unary operator
class UnaryExprAST : public ExprAST {
 public:
  TokenKind Op;
  ExprAST* Operand;  // non-owning; ASTContext owns the storage

  UnaryExprAST(TokenKind op, ExprAST* operand)
      : ExprAST(Kind::Unary), Op(op), Operand(operand) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "UnaryOp: " << tokenKindName(Op) << "\n";
    if (Operand) Operand->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Unary;
  }
};

// Binary operator
class BinaryExprAST : public ExprAST {
 public:
  TokenKind Op;
  ExprAST* LHS;
  ExprAST* RHS;

  BinaryExprAST(TokenKind op, ExprAST* lhs, ExprAST* rhs)
      : ExprAST(Kind::Binary), Op(op), LHS(lhs), RHS(rhs) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "BinaryOp: " << tokenKindName(Op) << "\n";
    if (LHS) LHS->print(indent + 1);
    if (RHS) RHS->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Binary;
  }
};

// Ternary operator: cond ? then : else
class TernaryExprAST : public ExprAST {
 public:
  ExprAST* Cond;
  ExprAST* ThenExpr;
  ExprAST* ElseExpr;

  TernaryExprAST(ExprAST* cond, ExprAST* then_, ExprAST* else_)
      : ExprAST(Kind::Ternary), Cond(cond), ThenExpr(then_), ElseExpr(else_) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Ternary\n";
    if (Cond)     Cond->print(indent + 1);
    if (ThenExpr) ThenExpr->print(indent + 1);
    if (ElseExpr) ElseExpr->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Ternary;
  }
};

// Boolean literal
class BooleanExprAST : public ExprAST {
 public:
  bool Val;

  explicit BooleanExprAST(bool val) : ExprAST(Kind::Boolean), Val(val) {}
  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Boolean: " << (Val ? "true" : "false") << "\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Boolean;
  }
};

// Member access or swizzle
class MemberAccessExprAST : public ExprAST {
 public:
  ExprAST* Object;
  std::string Member;

  MemberAccessExprAST(ExprAST* object, const std::string& member)
      : ExprAST(Kind::MemberAccess), Object(object), Member(member) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "MemberAccess: ." << Member << "\n";
    if (Object) Object->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::MemberAccess;
  }
};

// Member assignment
class MemberAssignmentExprAST : public ExprAST {
 public:
  ExprAST* Object;
  std::string Member;
  ExprAST* Init;

  MemberAssignmentExprAST(ExprAST* obj, std::string mem, ExprAST* init)
      : ExprAST(Kind::MemberAssignment),
        Object(obj),
        Member(std::move(mem)),
        Init(init) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "MemberAssignment: ." << Member << " = ...\n";
    if (Object) Object->print(indent + 1);
    if (Init)   Init->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::MemberAssignment;
  }
};

class MatrixAccessExprAST : public ExprAST {
 public:
  ExprAST* Object;
  ExprAST* Index;
  ExprAST* Index2;

  MatrixAccessExprAST(ExprAST* object, ExprAST* index, ExprAST* index2 = nullptr)
      : ExprAST(Kind::MatrixAccess),
        Object(object),
        Index(index),
        Index2(index2) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "MatrixAccess: []\n";
    if (Object) Object->print(indent + 1);
    if (Index)  Index->print(indent + 1);
    if (Index2) Index2->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::MatrixAccess;
  }
};

// Matrix element assignment: mat[i] = col_vec  or  mat[i][j] = scalar
class MatrixAssignmentExprAST : public ExprAST {
 public:
  ExprAST* Object;  // the matrix variable
  ExprAST* Index;   // column index
  ExprAST* Index2;  // row index (nullptr => whole-column assignment)
  ExprAST* RHS;

  MatrixAssignmentExprAST(ExprAST* obj, ExprAST* idx, ExprAST* idx2,
                          ExprAST* rhs)
      : ExprAST(Kind::MatrixAssignment),
        Object(obj),
        Index(idx),
        Index2(idx2),
        RHS(rhs) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "MatrixAssignment: [" << (Index2 ? "][" : "") << "] = ...\n";
    if (Object) Object->print(indent + 1);
    if (RHS)    RHS->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::MatrixAssignment;
  }
};

class CallExprAST : public ExprAST {
 public:
  std::string Callee;
  std::vector<ExprAST*> Args;

  CallExprAST(const std::string& callee, std::vector<ExprAST*> args)
      : ExprAST(Kind::Call), Callee(callee), Args(std::move(args)) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Call: " << Callee << "\n";
    for (auto* arg : Args)
      if (arg) arg->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Call;
  }
};

class IfExprAST : public ExprAST {
 public:
  ExprAST* Condition;
  ExprAST* ThenExpr;
  ExprAST* ElseExpr;  // może biti nullptr

  IfExprAST(ExprAST* cond, ExprAST* then, ExprAST* elseExpr = nullptr)
      : ExprAST(Kind::If),
        Condition(cond),
        ThenExpr(then),
        ElseExpr(elseExpr) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "If:\n";
    printIndent(indent + 1);
    std::cout << "Condition:\n";
    if (Condition) Condition->print(indent + 2);
    printIndent(indent + 1);
    std::cout << "Then:\n";
    if (ThenExpr) ThenExpr->print(indent + 2);
    if (ElseExpr) {
      printIndent(indent + 1);
      std::cout << "Else:\n";
      ElseExpr->print(indent + 2);
    }
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::If;
  }
};

class BlockExprAST : public ExprAST {
 public:
  std::vector<ExprAST*> Statements;

  explicit BlockExprAST(std::vector<ExprAST*> stmts)
      : ExprAST(Kind::Block), Statements(std::move(stmts)) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Block:\n";
    for (auto* stmt : Statements)
      if (stmt) stmt->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Block;
  }
};

// Assignment statement
class AssignmentExprAST : public ExprAST {
 public:
  std::string VarName;
  std::string VarType;  // npr. "vec3"
  ExprAST* Init;

  AssignmentExprAST(const std::string& type, const std::string& name,
                    ExprAST* init)
      : ExprAST(Kind::Assignment),
        VarName(name),
        VarType(type),
        Init(init) {}

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
    if (Init) Init->print(indent + 2);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Assignment;
  }
};

// Function prototype
class PrototypeAST : public ExprAST {
 public:
  std::string Name;
  std::vector<std::pair<std::string, std::string>> Args;  // {type, name}
  std::string RetType;

  PrototypeAST(std::string name,
               std::vector<std::pair<std::string, std::string>> args,
               std::string ret)
      : ExprAST(Kind::Prototype),
        Name(std::move(name)),
        Args(std::move(args)),
        RetType(std::move(ret)) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Proto: " << RetType << " " << Name << "(...)\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Prototype;
  }
};

// Function definition
class FunctionAST : public ExprAST {
 public:
  PrototypeAST* Proto;
  ExprAST* Body;
  FunctionAttrs Attrs;

  FunctionAST(PrototypeAST* proto, ExprAST* body, FunctionAttrs attrs = {})
      : ExprAST(Kind::Function),
        Proto(proto),
        Body(body),
        Attrs(std::move(attrs)) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Function: " << (Proto ? Proto->Name : "<no-proto>");

    if (Attrs.stage.has_value()) {
      std::cout << " [stage=";
      switch (*Attrs.stage) {
        case ShaderStage::Vertex:
          std::cout << "vertex";
          break;
        case ShaderStage::Fragment:
          std::cout << "fragment";
          break;
        case ShaderStage::Compute:
          std::cout << "compute";
          break;
      }
      std::cout << "]";
    }
    if (Attrs.isEntry) std::cout << " [entry]";
    std::cout << "\n";

    if (Body) Body->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Function;
  }
};

// Return statement
class ReturnStmtAST : public ExprAST {
 public:
  ExprAST* Expr;  // nullable: bare `return;`

  explicit ReturnStmtAST(ExprAST* e) : ExprAST(Kind::Return), Expr(e) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Return\n";
    if (Expr) Expr->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Return;
  }
};

// While statement

class WhileExprAST : public ExprAST {
 public:
  ExprAST* Condition;
  ExprAST* Body;

  WhileExprAST(ExprAST* Cond, ExprAST* B)
      : ExprAST(Kind::While), Condition(Cond), Body(B) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
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

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::While;
  }
};

// FOR STATEMENT

class ForExprAST : public ExprAST {
 public:
  ExprAST* Init;
  ExprAST* Condition;
  ExprAST* Increment;
  ExprAST* Body;

  ForExprAST(ExprAST* I, ExprAST* C, ExprAST* Inc, ExprAST* B)
      : ExprAST(Kind::For),
        Init(I),
        Condition(C),
        Increment(Inc),
        Body(B) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
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

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::For;
  }
};

// BREAK STATEMENT

class BreakStmtAST : public ExprAST {
 public:
  BreakStmtAST() : ExprAST(Kind::Break) {}
  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Break\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Break;
  }
};

// Continue statement
class ContinueStmtAST : public ExprAST {
 public:
  ContinueStmtAST() : ExprAST(Kind::Continue) {}
  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Continue\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Continue;
  }
};

// Discard statement (fragment stage only)
class DiscardStmtAST : public ExprAST {
 public:
  DiscardStmtAST() : ExprAST(Kind::Discard) {}
  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Discard\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::Discard;
  }
};

// STRUCT DECLARATION

class StructDeclExprAST : public ExprAST {
 public:
  std::string Name;
  std::vector<std::pair<std::string, std::string>> Fields;  // (type, name)

  StructDeclExprAST(const std::string& name,
                    std::vector<std::pair<std::string, std::string>> fields)
      : ExprAST(Kind::StructDecl), Name(name), Fields(std::move(fields)) {}
  // Create an opaque StructType placeholder and register the name in
  // NamedStructTypes (with empty field-name list). Used by the entry-point
  // driver as a *first* pass so codegen of later structs / functions can
  // resolve forward references like `struct A { B b; }; struct B { ... };`.
  void predeclare();
  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent + 2);
    std::cout << "StructDecl: " << Name << "\n";
    for (const auto& field : Fields) {
      printIndent(indent + 4);
      std::cout << "Field: " << field.first << " " << field.second << "\n";
    }
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::StructDecl;
  }
};

// Stage variable declaration: in/out type name;
class StageVarDeclAST : public ExprAST {
 public:
  bool isInput;  // true = "in", false = "out"
  std::string TypeName;
  std::string Name;
  int binding;  // -1 if no layout(binding=N)

  StageVarDeclAST(bool isInput_, std::string typeName, std::string name,
                  int binding_ = -1)
      : ExprAST(Kind::StageVarDecl),
        isInput(isInput_),
        TypeName(std::move(typeName)),
        Name(std::move(name)),
        binding(binding_) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent + 2);
    std::cout << (isInput ? "StageIn: " : "StageOut: ") << TypeName << " "
              << Name << "\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::StageVarDecl;
  }
};

// UNIFORM DECLARATION

class UniformDeclExprAST : public ExprAST {
 public:
  std::string TypeName;
  std::string Name;
  UniformDeclExprAST(const std::string& ty, const std::string& n)
      : ExprAST(Kind::UniformDecl), TypeName(ty), Name(n) {}
  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent + 2);
    std::cout << "UniformDecl: " << TypeName << " " << Name << "\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::UniformDecl;
  }
};

// ARRAY INITIALIZATION
class ArrayInitExprAST : public ExprAST {
 public:
  std::vector<ExprAST*> Elements;

  explicit ArrayInitExprAST(std::vector<ExprAST*> elements)
      : ExprAST(Kind::ArrayInit), Elements(std::move(elements)) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "ArrayInit: { ";
    for (size_t i = 0; i < Elements.size(); ++i) {
      if (Elements[i]) Elements[i]->print(0);
      if (i + 1 < Elements.size()) std::cout << ", ";
    }
    std::cout << " }\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::ArrayInit;
  }
};

// ARRAY DECLARATION

class ArrayDeclExprAST : public ExprAST {
 public:
  std::string ElementType;
  std::string Name;
  int Size;
  ExprAST* Init;

  ArrayDeclExprAST(const std::string& type, const std::string& name, int size,
                   ExprAST* init)
      : ExprAST(Kind::ArrayDecl),
        ElementType(type),
        Name(name),
        Size(size),
        Init(init) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "ArrayDecl: " << ElementType << " " << Name << "[" << Size
              << "]\n";
    if (Init) Init->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::ArrayDecl;
  }
};

// UNIFORM ARRAY DECLARATION

class UniformArrayDeclExprAST : public ExprAST {
 public:
  std::string TypeName;
  std::string Name;
  int Size;

  UniformArrayDeclExprAST(const std::string& ty, const std::string& n, int size)
      : ExprAST(Kind::UniformArrayDecl), TypeName(ty), Name(n), Size(size) {}

  llvm::Value* codegen() override;

  void print(int indent = 0) const override {
    printIndent(indent + 2);
    std::cout << "UniformArrayDecl: " << TypeName << " " << Name << "[" << Size
              << "]\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::UniformArrayDecl;
  }
};

// STORAGE BUFFER DECLARATION (compute shaders)
// layout(std430, binding=N) readonly/writeonly buffer ElemType name[];
class StorageBufferDeclAST : public ExprAST {
 public:
  std::string ElemType;  // e.g. "uint", "vec4"
  std::string Name;      // variable name visible in shader body
  bool isReadOnly;       // true = readonly, false = writeonly
  int binding;           // binding index

  StorageBufferDeclAST(std::string elemType, std::string name, bool ro,
                       int bind)
      : ExprAST(Kind::StorageBufferDecl),
        ElemType(std::move(elemType)),
        Name(std::move(name)),
        isReadOnly(ro),
        binding(bind) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent + 2);
    std::cout << "StorageBuffer: " << (isReadOnly ? "readonly" : "writeonly")
              << " " << ElemType << " " << Name << "[] (binding=" << binding
              << ")\n";
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::StorageBufferDecl;
  }
};

// Postfix increment / decrement: x++ / x--.
//
// Semantically distinct from prefix: codegen must store the new value but
// return the OLD value. A naive desugar to AssignmentExprAST(x, x±1) would
// give prefix semantics — `a[i++]` would index by the *new* `i`.
class PostfixIncrExprAST : public ExprAST {
 public:
  ExprAST* Target;  // currently restricted to VariableExprAST
  bool isDecrement;

  PostfixIncrExprAST(ExprAST* target, bool dec)
      : ExprAST(Kind::PostfixIncr), Target(target), isDecrement(dec) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "Postfix" << (isDecrement ? "Decr" : "Incr") << "\n";
    if (Target) Target->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::PostfixIncr;
  }
};

// Implicit conversion inserted by SemanticAnalyzer (GLSL §4.1.10), e.g. the
// int→float promotion in `1.0 + 2`. The target type is carried in ExprAST::Ty
// (set by the constructor). Making conversions explicit AST nodes — Clang's
// ImplicitCastExpr — means later passes never face a silent type mismatch.
//
// Step 3: codegen() is a transparent pass-through; the legacy on-the-fly
// conversions in the surrounding codegen still do the real work. Step 4 moves
// that lowering here (sitofp / zext / splat / …), keyed on the target type.
class ImplicitCastExprAST : public ExprAST {
 public:
  ExprAST* Operand;  // the value being converted (non-owning)

  ImplicitCastExprAST(ExprAST* operand, const glsl::Type* target)
      : ExprAST(Kind::ImplicitCast), Operand(operand) {
    setType(target);
  }

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "ImplicitCast -> "
              << (getType() ? getType()->toString() : "?") << "\n";
    if (Operand) Operand->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::ImplicitCast;
  }
};

#endif  // AST_H
