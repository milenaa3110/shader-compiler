// sema/sema.h — post-parse semantic analysis.
//
// The parser does pure syntax. This pass walks the AST and enforces type-
// level invariants the parser deliberately defers:
//
//   * collect struct declarations and detect cyclic field dependencies
//     (direct or indirect), and
//   * validate that every type-name string referenced by a node resolves
//     to a builtin scalar/vector/matrix/sampler/image or to a struct
//     declared in this program.
//
// Errors are reported through the same `logErrorAt` channel the parser
// uses, so diagnostics share a single pipeline. `run()` returns the
// number of errors emitted; callers should refuse to proceed to codegen
// when nonzero.
//
// `structNames()` exposes the collected set so codegen can rely on the
// same authoritative list (no shared globals).

#ifndef SEMA_H
#define SEMA_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ast/ast.h"

class ASTContext;  // fwd — used to mint canonical types (see ast_context.h)

class SemanticAnalyzer {
 public:
  // The analyzer mints canonical types (struct types, literal types, …) from
  // the same ASTContext that owns the AST nodes.
  explicit SemanticAnalyzer(ASTContext& ctx) : Ctx(ctx) {}

  // Run all checks. Returns total error count.
  int run(const std::vector<ExprAST*>& program);

  const std::unordered_set<std::string>& structNames() const {
    return structNames_;
  }

 private:
  // Pass 1: gather every `struct X { ... };` name.
  void collectStructNames(
      const std::vector<ExprAST*>& program);

  // Pass 2: for each known struct, collect the subset of field types that
  // are themselves known structs — the edges of the dependency graph.
  void buildDependencies(
      const std::vector<ExprAST*>& program);

  // Pass 3: DFS over dependency graph; reports each cycle once.
  void detectCycles();

  // Pass 4: walk the program; for every node that carries a type-name
  // string, verify it resolves to a builtin or a known struct.
  void validateTypeRefs(
      const std::vector<ExprAST*>& program);

  // Visit helpers — recursive descent over a single subtree. Non-const: it
  // stamps each node's semantic type (setType) as it goes.
  void visit(ExprAST* node);

  // Type a numeric literal (int/uint/float) and range-check it (GLSL §4.1).
  void typeNumberLiteral(NumberExprAST* num);

  // ── Type inference (Step 2 / Slice B) ──────────────────────────────────────
  // Map a declaration's type-name string to the canonical type, or nullptr if
  // the name isn't a builtin or a known struct.
  const glsl::Type* resolveTypeName(llvm::StringRef name);
  // Bind top-level names (uniforms, stage vars, buffers) into the global scope
  // and record each function's return type, so bodies can reference them
  // regardless of declaration order.
  void bindGlobals(const std::vector<ExprAST*>& program);
  // Type an expression subtree and return its type (also stamped on the node).
  // Best-effort: returns nullptr where the rule isn't settled yet (composite
  // matrix ops, builtins) — no type-mismatch diagnostics here; those land in
  // the conversion step. Never errors except the literal range-check.
  const glsl::Type* typeExpr(ExprAST* node);
  const glsl::Type* inferBinaryType(TokenKind op, const glsl::Type* lhs,
                                    const glsl::Type* rhs);
  // Common type both operands of an arithmetic/relational binary op convert to
  // (GLSL §5): scalar∘scalar → higher rank; scalar∘vector and vector∘vector
  // (same size) → a vector whose element is the higher-rank element. Returns
  // nullptr for the unsettled cases (matrices, mismatched sizes, a double/bool
  // element that has no vector form) — those stay on the codegen net for now.
  const glsl::Type* commonOperandType(const glsl::Type* l, const glsl::Type* r);
  // Result type of a matrix binary op (called from inferBinaryType BEFORE
  // commonOperandType, whenever either operand is a matrix). Returns the GLSL
  // result type when the op is dimensionally legal (mat*vec→vecR, vec*mat→vecC,
  // mat*mat→mat, mat*scalar→mat, mat±mat→mat), else nullptr — and a nullptr here
  // is NOT a "defer": checkOperandTypes diagnoses the illegal case (it shares
  // the matrixDimsOK predicate), so the node stays untyped but an error is
  // emitted. Lowering is a separate (later) piece; this is sema-only.
  const glsl::Type* inferMatrixBinary(TokenKind op, const glsl::Type* l,
                                      const glsl::Type* r);
  // Result type of a builtin call (sin/dot/texture/mix/…) from its signature,
  // given the already-typed argument types; nullptr for an unknown builtin (it
  // stays untyped and the codegen net resolves it). genType builtins take the
  // shape of their value argument (the first vector arg, else arg0).
  const glsl::Type* typeBuiltinCall(const std::string& name,
                                    const std::vector<const glsl::Type*>& args);
  const glsl::Type* typeMember(const glsl::Type* objTy,
                               const std::string& member);
  const glsl::Type* indexOnce(const glsl::Type* aggregate);

  // ASSIGNMENT-context coercion (§4.1.10: initialization / assignment / return /
  // argument / ternary arm). If `e` converts implicitly to `target` (scalar
  // widening, or same-shape component-wise vector/matrix widening — NO
  // scalar→vector, NO bool), wrap it in an ImplicitCastExprAST; else unchanged.
  ExprAST* coerce(ExprAST* e, const glsl::Type* target);

  // OPERATOR-context coercion (§5): everything coerce() allows, PLUS a scalar
  // broadcasting to a vector whose element it widens to (`w * 2.0`). Used only
  // for binary operands, never for assignment/return/argument.
  ExprAST* coerceOperand(ExprAST* e, const glsl::Type* target);

  // Diagnose a value of type `from` used where `to` is required (assignment
  // context) when no implicit conversion exists — narrowing (float→int), a
  // scalar where a vector is needed (float→vec3 must be written vec3(x)), a
  // vector-size or element mismatch, bool↔numeric, struct mismatch. `role` names
  // the context ("initialization", "return", "argument", …).
  void checkConvertible(const glsl::Type* from, const glsl::Type* to,
                        SourceLocation loc, const char* role);

  // Reject operand types a binary operator doesn't accept (the binary analogue
  // of the unary `!` bool check): arithmetic/relational want numeric operands
  // (bool/sampler/struct rejected), bitwise/shift want integers.
  void checkOperandTypes(TokenKind op, const glsl::Type* l, const glsl::Type* r,
                         SourceLocation loc);

  // Lexical scope stack for variable types (scopes_.front() is the globals).
  void enterScope() { scopes_.emplace_back(); }
  void leaveScope() { scopes_.pop_back(); }
  void bindLocal(const std::string& name, const glsl::Type* t);
  const glsl::Type* lookupVar(const std::string& name) const;

  // Checks.
  bool isBuiltinType(const std::string& name) const;
  bool isKnownType(const std::string& name) const;
  void checkTypeRef(const std::string& name, SourceLocation loc,
                    const char* role);

  // DFS state for detectCycles().
  bool hasCycleFrom(const std::string& start,
                    std::unordered_set<std::string>& visited,
                    std::unordered_set<std::string>& onStack);

  ASTContext& Ctx;  // owns the AST + mints canonical types

  // Lexical scopes: scopes_[0] is the globals (uniforms/stage vars/buffers),
  // one frame pushed per function body / block / for-loop. Maps name → type.
  std::vector<std::unordered_map<std::string, const glsl::Type*>> scopes_;
  // Each user function's signature, by name: return type (for typing a call) and
  // parameter types (for coercing call arguments). nullptr entries are allowed
  // (e.g. a void return or an unresolved param type).
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
