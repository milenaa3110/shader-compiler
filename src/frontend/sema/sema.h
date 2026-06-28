// sema/sema.h — post-parse semantic analysis.

//   collect struct declarations and detect cyclic field dependencies
//   (direct or indirect), and
//   validate that every type-name string referenced by a node resolves
//   to a builtin scalar/vector/matrix/sampler/image or to a struct
//   declared in this program.

#ifndef SEMA_H
#define SEMA_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ast/ast.h"

class ASTContext;

class SemanticAnalyzer {
 public:
  explicit SemanticAnalyzer(ASTContext& ctx) : Ctx(ctx) {}

  // Run all checks. Returns total error count.
  int run(const std::vector<ExprAST*>& program);

  const std::unordered_set<std::string>& structNames() const {
    return structNames_;
  }

 private:
  // gather every `struct X { ... };` name.
  void collectStructNames(
      const std::vector<ExprAST*>& program);

  // for each known struct, collect the subset of field types that
  // are themselves known structs — the edges of the dependency graph.
  void buildDependencies(
      const std::vector<ExprAST*>& program);

  // DFS over dependency graph; reports each cycle once.
  void detectCycles();

  // walk the program; for every node that carries a type-name
  // string, verify it resolves to a builtin or a known struct.
  void validateTypeRefs(
      const std::vector<ExprAST*>& program);

  // Replicate backend input/output slot allocation and detect location aliasing.
  void checkStageVarLocations(const std::vector<ExprAST*>& program);

  // Visit helpers — recursive descent over a single subtree. 
  // Non-const: it stamps each node's semantic type (setType) as it goes.
  void visit(ExprAST* node);

  // Type a numeric literal (int/uint/float) and range-check it
  void typeNumberLiteral(NumberExprAST* num);

  // Map a declaration's type-name string to the canonical type, or nullptr if
  // the name isn't a builtin or a known struct.
  const glsl::Type* resolveTypeName(llvm::StringRef name);

 // Bind global declarations and function signatures to support out-of-order resolution.
  void bindGlobals(const std::vector<ExprAST*>& program);

  // Infer the semantic type of an expression subtree and stamp it onto the node.
  const glsl::Type* typeExpr(ExprAST* node);
  const glsl::Type* inferBinaryType(TokenKind op, const glsl::Type* lhs,
                                    const glsl::Type* rhs);

  // Compute the standard GLSL promotion type for arithmetic/relational operands.
  const glsl::Type* commonOperandType(const glsl::Type* l, const glsl::Type* r);

  // Deduce the result type of a dimensionally legal matrix binary operation.
  const glsl::Type* inferMatrixBinary(TokenKind op, const glsl::Type* l,
                                      const glsl::Type* r);

  // Resolve polymorphism for builtins (e.g., genType) based on argument shapes.
  const glsl::Type* typeBuiltinCall(const std::string& name,
                                    const std::vector<const glsl::Type*>& args);
  const glsl::Type* typeMember(const glsl::Type* objTy,
                               const std::string& member);
  const glsl::Type* indexOnce(const glsl::Type* aggregate);

  // Apply assignment-context implicit conversions by injecting an ImplicitCastExprAST.
  ExprAST* coerce(ExprAST* e, const glsl::Type* target);

  // Apply operator-context implicit conversions, including scalar broadcasting.
  ExprAST* coerceOperand(ExprAST* e, const glsl::Type* target);

  // Diagnose illegal implicit type conversions in assignment contexts.
  // 'role' specifies the semantic context (e.g., "initialization", "return")
  void checkConvertible(const glsl::Type* from, const glsl::Type* to,
                        SourceLocation loc, const char* role);

  // Validate that operand types are compatible with the binary operator kind.
  void checkOperandTypes(TokenKind op, const glsl::Type* l, const glsl::Type* r,
                         SourceLocation loc);

  // A subscript index (`a[i]`, `m[i][j]`) must be integral (int/uint); reject if not
  void checkIndexType(ExprAST* idx);

  // Lexical scope stack for variable types (scopes_.front() is the globals).
  void enterScope() { scopes_.emplace_back(); }
  void leaveScope() { scopes_.pop_back(); }
  void bindLocal(const std::string& name, const glsl::Type* t,
                 bool isConst = false);
  const glsl::Type* lookupVar(const std::string& name) const;
  // Check if a symbol is declared as a compile-time constant
  bool isConstVar(const std::string& name) const;
  // Diagnose mutations on const data by unwrapping member and index access chains.
  bool reportIfConstWrite(const ExprAST* target, SourceLocation loc);

  // Checks.
  bool isBuiltinType(const std::string& name) const;
  bool isKnownType(const std::string& name) const;
  void checkTypeRef(const std::string& name, SourceLocation loc,
                    const char* role);

  // DFS state for detectCycles().
  bool hasCycleFrom(const std::string& start,
                    std::unordered_set<std::string>& visited,
                    std::unordered_set<std::string>& onStack);

  ASTContext& Ctx;

  // Per-variable scope entry: its type plus whether it was declared `const`.
  struct VarInfo {
    const glsl::Type* type = nullptr;
    bool isConst = false;
  };
  // Stack of lexical scopes (one frame per block/loop). scopes_[0] is global
  std::vector<std::unordered_map<std::string, VarInfo>> scopes_;
  // User function signature. Param/return types may be nullptr if unresolved
  struct FuncSig {
    const glsl::Type* ret = nullptr;
    std::vector<const glsl::Type*> params;
  };
  std::unordered_map<std::string, FuncSig> funcSignatures_;
  // Return type of the function currently being walked (for `return` coercion).
  const glsl::Type* currentRet_ = nullptr;

  std::unordered_set<std::string> structNames_;
  std::unordered_map<std::string, std::vector<std::string>>
      structDependencies_;
  // Source position of each struct decl, for cycle diagnostics.
  std::unordered_map<std::string, SourceLocation> structLocs_;

  int errorCount_ = 0;
};

#endif  // SEMA_H
