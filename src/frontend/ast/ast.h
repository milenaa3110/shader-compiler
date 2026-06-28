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

// AST ownership model
//
// All AST nodes are non-owning and allocated via an ASTContext bump pool.
// Context destruction performs a single reverse-walk to invoke virtual 
// destructors (clearing node-owned members like std::vector/std::string) 
// before freeing the underlying arena. Subclasses must forward their 
// ExprAST::Kind to enable high-performance LLVM-style RTTI casting.

// Shader stage and function attributes
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
// Uses LLVM-style RTTI via a single-byte discriminator ('Kind') passed to 
// the base constructor. This avoids C++ vtable/dynamic_cast performance 
// overhead, enabling O(1) type evaluation via llvm::isa/cast/dyn_cast.
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
    IncrDecr,
    ImplicitCast,
    RawValue,
  };

  virtual ~ExprAST() = default;
  virtual llvm::Value* codegen() = 0;
  virtual void print(int indent = 0) const = 0;

  Kind getKind() const { return kind_; }

  // Source location of the construct's first token, stamped by Parser::at().
  // An invalid/default location signals an unstamped node, defaulting 
  // diagnostic fallback to "line 0, col 0" to flag missed parse paths.
  SourceLocation loc;

  // The semantic type of this expression, resolved by SemanticAnalyzer.
  // nullptr until type-checked; callers must null-check during partial AST passes.
  // Guaranteed non-null for all value-producing nodes prior to code generation.
  const glsl::Type* getType() const { return Ty; }
  void setType(const glsl::Type* t) { Ty = t; }

 protected:
  // No default constructor: every subclass MUST forward its Kind. 
  // A missed subclass becomes a compile error
  explicit ExprAST(Kind k) : kind_(k) {}

 private:
  Kind kind_;
  const glsl::Type* Ty = nullptr;  // set by SemanticAnalyzer
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
  ExprAST* ElseExpr;  // can be nullptr

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
  // True when the declaration carried a `const` qualifier (`const int x = 3;`)
  bool IsConst = false;

  AssignmentExprAST(const std::string& type, const std::string& name,
                    ExprAST* init, bool isConst = false)
      : ExprAST(Kind::Assignment),
        VarName(name),
        VarType(type),
        Init(init),
        IsConst(isConst) {}

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
  // Parameter direction qualifiers, mapping 1:1 to Args (0=in, 1=out, 2=inout).
  // An empty vector implicitly treats all parameters as 'in' (by-value).
  std::vector<uint8_t> ArgDir;

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
  // Registers an opaque, empty llvm::StructType placeholder within the symbol 
  // table prior to full emission. This initial pass allows subsequent type 
  // definitions and function signatures to successfully resolve forward references.
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
  int location;  // -1 if no layout(location=N); the interstage IO slot

  StageVarDeclAST(bool isInput_, std::string typeName, std::string name,
                  int location_ = -1)
      : ExprAST(Kind::StageVarDecl),
        isInput(isInput_),
        TypeName(std::move(typeName)),
        Name(std::move(name)),
        location(location_) {}

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
  // The layout(binding=N) slot index; holds -1 if unspecified.
  // Used to assign Vulkan/SPIR-V descriptor bindings for sampler and image 
  // uniforms. Plain scalar uniforms bypass this to leverage push constants.
  int binding;

  UniformDeclExprAST(const std::string& ty, const std::string& n,
                     int bind = -1)
      : ExprAST(Kind::UniformDecl), TypeName(ty), Name(n), binding(bind) {}
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
  bool IsConst = false;  // declared `const` — see AssignmentExprAST::IsConst.

  ArrayDeclExprAST(const std::string& type, const std::string& name, int size,
                   ExprAST* init, bool isConst = false)
      : ExprAST(Kind::ArrayDecl),
        ElementType(type),
        Name(name),
        Size(size),
        Init(init),
        IsConst(isConst) {}

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
  int binding;  // layout(binding=N); -1 if unspecified. See UniformDeclExprAST.

  UniformArrayDeclExprAST(const std::string& ty, const std::string& n, int size,
                          int bind = -1)
      : ExprAST(Kind::UniformArrayDecl),
        TypeName(ty),
        Name(n),
        Size(size),
        binding(bind) {}

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

// Handles increment/decrement side-effect expressions (++x, x--, --x, x++)
// targeting variables. Both axes are flags: isPrefix (pre/post) and
// isDecrement (++/--).
class IncrDecrExprAST : public ExprAST {
 public:
  ExprAST* Target;  // VariableExprAST / MemberAccessExprAST / MatrixAccessExprAST
  bool isDecrement;
  bool isPrefix;

  IncrDecrExprAST(ExprAST* target, bool dec, bool prefix = false)
      : ExprAST(Kind::IncrDecr),
        Target(target),
        isDecrement(dec),
        isPrefix(prefix) {}

  llvm::Value* codegen() override;
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << (isPrefix ? "Prefix" : "Postfix")
              << (isDecrement ? "Decr" : "Incr") << "\n";
    if (Target) Target->print(indent + 1);
  }

  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::IncrDecr;
  }
};

// A transient node used to inject a pre-lowered llvm::Value* into existing 
// AST emission logic (e.g., passing modified values to internal assignment 
// handlers). Bypasses parsing and semantic analysis phases.
class RawValueExprAST : public ExprAST {
 public:
  llvm::Value* V;
  explicit RawValueExprAST(llvm::Value* v) : ExprAST(Kind::RawValue), V(v) {}
  llvm::Value* codegen() override { return V; }
  void print(int indent = 0) const override {
    printIndent(indent);
    std::cout << "RawValue\n";
  }
  static bool classof(const ExprAST* e) {
    return e->getKind() == Kind::RawValue;
  }
};

// Wraps operands undergoing implicit conversion (e.g., int-to-float promotion).
// Prevents type mismatch bugs in downstream components by rendering compiler-
// generated data type conversions explicit within the AST.
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
