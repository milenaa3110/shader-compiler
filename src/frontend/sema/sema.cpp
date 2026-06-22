// sema/sema.cpp — implementation of the post-parse semantic pass.

#include "sema.h"

#include <fmt/core.h>

#include "../../common/error_utils_fmt.h"
#include "../ast/ast_context.h"

// sema.cpp does not pull bare `llvm::Type` into scope, so aliasing the semantic
// type keeps the typing code readable.
using glsl::Type;

// ────────────────────────────────────────────────────────────────────────────
// The set of types the lexer recognises as keywords — kept in sync with
// `tokToTypeName()` in the parser. Anything outside this set MUST be a
// user-declared struct.
// ────────────────────────────────────────────────────────────────────────────
bool SemanticAnalyzer::isBuiltinType(const std::string& name) const {
  static const std::unordered_set<std::string> kBuiltins = {
      "void",  // return-only; every value type comes from builtin_types.def
#define BTYPE(Tok, Spelling) Spelling,
#include "../ast/builtin_types.def"
  };
  return kBuiltins.count(name) > 0;
}

bool SemanticAnalyzer::isKnownType(const std::string& name) const {
  return isBuiltinType(name) || structNames_.count(name) > 0;
}

void SemanticAnalyzer::checkTypeRef(const std::string& name, SourceLocation loc,
                                    const char* role) {
  if (isKnownType(name)) return;
  logErrorAt(loc, fmt::format("Unknown {} type '{}'", role, name));
  ++errorCount_;
}

// Bridge from a declaration's type-name string to the canonical glsl::Type.
// Returns nullptr for an unknown name (the caller already diagnoses those via
// checkTypeRef, so this stays silent).
const Type* SemanticAnalyzer::resolveTypeName(llvm::StringRef n) {
  if (n == "void") return Ctx.getVoidTy();  // return-only; not in the .def
  using SK = Type::SamplerKind;
  // Every value-bearing builtin from builtin_types.def — the same source the
  // lexer/parser/codegen use, so the lists can't drift.
#define BTYPE_SCALAR(Tok, Spelling, GlslKind, LlvmGetter) \
  if (n == Spelling) return Ctx.get##GlslKind##Ty();
#define BTYPE_VECTOR(Tok, Spelling, GlslElem, LlvmElem, N) \
  if (n == Spelling) return Ctx.getVectorTy(Ctx.get##GlslElem##Ty(), N);
#define BTYPE_MATRIX(Tok, Spelling, Cols, Rows) \
  if (n == Spelling) return Ctx.getMatrixTy(Cols, Rows);
#define BTYPE_SAMPLER(Tok, Spelling, Kind, Dim, Arrayed, IsImage) \
  if (n == Spelling) return Ctx.getSamplerTy(SK::Kind);
#include "../ast/builtin_types.def"
  // user struct (only after collectStructNames has run)
  if (structNames_.count(std::string(n))) return Ctx.getStructTy(n);
  return nullptr;
}

// ── Scope helpers ───────────────────────────────────────────────────────────
void SemanticAnalyzer::bindLocal(const std::string& name, const Type* t) {
  if (t && !scopes_.empty()) scopes_.back()[name] = t;
}
const Type* SemanticAnalyzer::lookupVar(const std::string& name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto f = it->find(name);
    if (f != it->end()) return f->second;
  }
  return nullptr;
}

// Bind top-level declarations into the global scope and record function return
// types, so a body can reference a uniform / function declared later.
void SemanticAnalyzer::bindGlobals(const std::vector<ExprAST*>& program) {
  for (auto* node : program) {
    if (!node) continue;
    using K = ExprAST::Kind;
    switch (node->getKind()) {
      case K::UniformDecl: {
        auto* u = llvm::cast<UniformDeclExprAST>(node);
        bindLocal(u->Name, resolveTypeName(u->TypeName));
        break;
      }
      case K::UniformArrayDecl: {
        auto* u = llvm::cast<UniformArrayDeclExprAST>(node);
        if (const Type* e = resolveTypeName(u->TypeName))
          bindLocal(u->Name, Ctx.getArrayTy(e, static_cast<unsigned>(u->Size)));
        break;
      }
      case K::StageVarDecl: {
        auto* s = llvm::cast<StageVarDeclAST>(node);
        bindLocal(s->Name, resolveTypeName(s->TypeName));
        break;
      }
      case K::StorageBufferDecl: {
        auto* b = llvm::cast<StorageBufferDeclAST>(node);
        if (const Type* e = resolveTypeName(b->ElemType))
          bindLocal(b->Name, Ctx.getUnsizedArrayTy(e));  // SSBO `name[]`
        break;
      }
      case K::Function: {
        auto* fn = llvm::cast<FunctionAST>(node);
        if (fn->Proto) {
          FuncSig sig;
          sig.ret = resolveTypeName(fn->Proto->RetType);  // nullptr for void
          for (const auto& [tyName, _argName] : fn->Proto->Args)
            sig.params.push_back(resolveTypeName(tyName));
          funcSignatures_[fn->Proto->Name] = std::move(sig);
        }
        break;
      }
      default:
        break;
    }
  }
}

// ── Pass 1 ─────────────────────────────────────────────────────────────────
// Collect struct names and complete their canonical types. Completion uses the
// incomplete-type protocol (type.h): getStructTy() returns the one canonical
// instance, incomplete (decl_ == nullptr) on first reference; a second
// definition is a user-diagnosed redefinition that must NOT reach
// setStructDecl. The redefinition gate IS the isIncompleteStruct() check.
void SemanticAnalyzer::collectStructNames(
    const std::vector<ExprAST*>& program) {
  for (auto* node : program) {
    auto* sd = llvm::dyn_cast_or_null<StructDeclExprAST>(node);
    if (!sd) continue;

    const Type* st = Ctx.getStructTy(sd->Name);
    if (!st->isIncompleteStruct()) {
      // Second `struct X { ... };` — diagnose, keep the first definition.
      logErrorAt(sd->loc,
                 fmt::format("redefinition of 'struct {}'", sd->Name));
      logErrorAt(st->structDecl()->loc,
                 fmt::format("previous definition of 'struct {}' is here",
                             sd->Name));
      ++errorCount_;
      continue;
    }
    structNames_.insert(sd->Name);
    structLocs_[sd->Name] = sd->loc;
    st->setStructDecl(sd);  // complete the type
  }
}

// ── Pass 2 ─────────────────────────────────────────────────────────────────
void SemanticAnalyzer::buildDependencies(
    const std::vector<ExprAST*>& program) {
  for (auto* node : program) {
    auto* sd = llvm::dyn_cast_or_null<StructDeclExprAST>(node);
    if (!sd) continue;
    std::vector<std::string> deps;
    for (const auto& [fieldType, fieldName] : sd->Fields) {
      // Built-ins terminate dependency walks; unknown names are caught by
      // validateTypeRefs and contribute no edge here.
      if (structNames_.count(fieldType) > 0) deps.push_back(fieldType);
    }
    if (!deps.empty()) structDependencies_[sd->Name] = std::move(deps);
  }
}

// ── Pass 3 ─────────────────────────────────────────────────────────────────
bool SemanticAnalyzer::hasCycleFrom(
    const std::string& start,
    std::unordered_set<std::string>& visited,
    std::unordered_set<std::string>& onStack) {
  visited.insert(start);
  onStack.insert(start);

  auto it = structDependencies_.find(start);
  if (it != structDependencies_.end()) {
    for (const auto& dep : it->second) {
      if (visited.find(dep) == visited.end()) {
        if (hasCycleFrom(dep, visited, onStack)) return true;
      } else if (onStack.count(dep)) {
        return true;
      }
    }
  }
  onStack.erase(start);
  return false;
}

void SemanticAnalyzer::detectCycles() {
  for (const auto& name : structNames_) {
    std::unordered_set<std::string> visited, onStack;
    if (hasCycleFrom(name, visited, onStack)) {
      auto it = structLocs_.find(name);
      SourceLocation loc =
          it != structLocs_.end() ? it->second : SourceLocation{};
      logErrorAt(loc,
                 fmt::format("Struct '{}' contains recursive definition",
                             name));
      ++errorCount_;
      // Erasing the dep prevents further DFS from re-reporting the same
      // cycle from a different start node.
      structDependencies_.erase(name);
    }
  }
}

// Rank for the implicit-conversion order int < uint < float < double (GLSL
// §4.1.10). Used to pick the result type of a scalar∘scalar mix.
static int scalarRank(const glsl::Type* t) {
  if (t->isBool())   return 0;
  if (t->isInt())    return 1;
  if (t->isUint())   return 2;
  if (t->isFloat())  return 3;
  if (t->isDouble()) return 4;
  return -1;
}
static const glsl::Type* higherRankScalar(const glsl::Type* a,
                                          const glsl::Type* b) {
  return scalarRank(a) >= scalarRank(b) ? a : b;
}
static bool isBoolResultOp(TokenKind op) {
  switch (op) {
    case TokenKind::Equal:
    case TokenKind::NotEqual:
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
    case TokenKind::And:
    case TokenKind::Or:
      return true;
    default:
      return false;
  }
}

static bool isShift(TokenKind op) {
  return op == TokenKind::ShiftLeft || op == TokenKind::ShiftRight;
}

// GLSL §4.1.10 implicit conversions are widening only (int→uint→float→double);
// bool never participates. Same type counts as convertible.
static bool canWiden(const glsl::Type* from, const glsl::Type* to) {
  auto numeric = [](const glsl::Type* t) {
    return t->isInt() || t->isUint() || t->isFloat() || t->isDouble();
  };
  return numeric(from) && numeric(to) && scalarRank(from) <= scalarRank(to);
}

// §4.1.10 implicit conversion in the ASSIGNMENT context (init/assign/return/
// argument/ternary arm): scalar→scalar widening, and component-wise widening of
// a vector to the SAME size. There is NO scalar→vector here — that broadcast is
// operator-only (§5) — and bool never converts (canWiden excludes it). Identity
// counts. Matrices are all float, so a same-shape matrix conversion is only
// identity (caught by from==to); differing shapes never convert.
static bool convertsImplicitly(const glsl::Type* from, const glsl::Type* to) {
  if (!from || !to) return false;
  if (from == to) return true;
  if (from->isScalar() && to->isScalar()) return canWiden(from, to);
  if (from->isVector() && to->isVector() &&
      from->vectorSize() == to->vectorSize())
    return canWiden(from->elementType(), to->elementType());
  return false;
}

// Operators whose scalar operands get promoted to their common type: arithmetic,
// the relational/equality comparisons, and the bitwise &|^ (GLSL gives those the
// usual int↔uint conversion, e.g. `int & uint → uint`). Logical (&&/||) want bool
// operands; SHIFTS are deliberately excluded — their result type follows the LHS
// (§5.9), so coercing the operands to a common type would be wrong.
static bool coercesOperands(TokenKind op) {
  switch (op) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Multiply:
    case TokenKind::Divide:
    case TokenKind::Percent:
    case TokenKind::Equal:
    case TokenKind::NotEqual:
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
    case TokenKind::BitwiseAnd:
    case TokenKind::BitwiseOr:
    case TokenKind::BitwiseXor:
      return true;
    default:
      return false;
  }
}

// ── Pass 4 ─────────────────────────────────────────────────────────────────
// Statement / declaration walk: manages lexical scopes, binds locals into the
// symbol table, validates type-name strings (checkTypeRef), and hands every
// expression to typeExpr. No type-mismatch diagnostics here — that is the
// conversion step (Step 3).
void SemanticAnalyzer::visit(ExprAST* node) {
  if (!node) return;
  using K = ExprAST::Kind;

  switch (node->getKind()) {
    case K::Function: {
      auto* fn = llvm::cast<FunctionAST>(node);
      // PrototypeAST isn't position-stamped by the parser; fall back to
      // the enclosing function's position so diagnostics land on the
      // `fn` keyword instead of an invalid location.
      const SourceLocation loc =
          fn->Proto && fn->Proto->loc.isValid() ? fn->Proto->loc : fn->loc;
      enterScope();
      const Type* savedRet = currentRet_;
      currentRet_ = nullptr;
      if (fn->Proto) {
        const auto& ret = fn->Proto->RetType;
        if (!ret.empty() && !isKnownType(ret)) {
          checkTypeRef(ret, loc, "return");
        }
        currentRet_ = resolveTypeName(ret);
        for (const auto& [ty, name] : fn->Proto->Args) {
          checkTypeRef(ty, loc, "parameter");
          bindLocal(name, resolveTypeName(ty));
        }
      }
      // Bind the stage builtins codegen synthesizes as entry params/outputs.
      // Without this they'd be untyped, so e.g. `gl_VertexID * 2.0` would skip
      // the int→float materialization and hit a codegen type mismatch.
      if (fn->Attrs.isEntry && fn->Attrs.stage) {
        const Type* uvec3 = Ctx.getVectorTy(Ctx.getUintTy(), 3);
        switch (*fn->Attrs.stage) {
          case ShaderStage::Vertex:
            bindLocal("gl_VertexID", Ctx.getIntTy());
            bindLocal("gl_InstanceID", Ctx.getIntTy());
            bindLocal("gl_Position", Ctx.getVec4Ty());
            break;
          case ShaderStage::Fragment:
            bindLocal("gl_FragCoord", Ctx.getVec4Ty());
            break;
          case ShaderStage::Compute:
            bindLocal("gl_GlobalInvocationID", uvec3);
            bindLocal("gl_LocalInvocationID", uvec3);
            bindLocal("gl_WorkGroupID", uvec3);
            bindLocal("gl_NumWorkgroups", uvec3);
            break;
        }
      }
      visit(fn->Body);
      currentRet_ = savedRet;
      leaveScope();
      return;
    }
    case K::Block: {
      enterScope();
      for (auto* s : llvm::cast<BlockExprAST>(node)->Statements) visit(s);
      leaveScope();
      return;
    }
    case K::If: {
      auto* i = llvm::cast<IfExprAST>(node);
      typeExpr(i->Condition);
      visit(i->ThenExpr);
      visit(i->ElseExpr);
      return;
    }
    case K::While: {
      auto* w = llvm::cast<WhileExprAST>(node);
      typeExpr(w->Condition);
      visit(w->Body);
      return;
    }
    case K::For: {
      auto* f = llvm::cast<ForExprAST>(node);
      enterScope();  // the loop variable is scoped to the loop
      visit(f->Init);
      typeExpr(f->Condition);
      visit(f->Increment);
      visit(f->Body);
      leaveScope();
      return;
    }
    case K::Assignment: {
      auto* a = llvm::cast<AssignmentExprAST>(node);
      // VarType is empty for pure re-assignments (`a = 5;`); non-empty only
      // for declarations carried through this node (`vec3 a = ...;`).
      typeExpr(a->Init);
      if (!a->VarType.empty()) {
        checkTypeRef(a->VarType, a->loc, "variable");
        const Type* target = resolveTypeName(a->VarType);
        bindLocal(a->VarName, target);
        checkConvertible(a->Init ? a->Init->getType() : nullptr, target, a->loc,
                         "initialization");
        a->Init = coerce(a->Init, target);  // widen initializer to decl type
      } else if (const Type* target = lookupVar(a->VarName)) {
        // Pure re-assignment (`f = 2;`): widen the RHS to the variable's type and
        // reject an illegal implicit narrowing — symmetric to the declaration
        // branch above.
        checkConvertible(a->Init ? a->Init->getType() : nullptr, target, a->loc,
                         "assignment");
        a->Init = coerce(a->Init, target);
      }
      return;
    }
    case K::ArrayDecl: {
      auto* a = llvm::cast<ArrayDeclExprAST>(node);
      checkTypeRef(a->ElementType, a->loc, "array element");
      typeExpr(a->Init);
      if (const Type* e = resolveTypeName(a->ElementType))
        bindLocal(a->Name, Ctx.getArrayTy(e, static_cast<unsigned>(a->Size)));
      return;
    }
    case K::UniformDecl:
      checkTypeRef(llvm::cast<UniformDeclExprAST>(node)->TypeName, node->loc,
                   "uniform");
      return;
    case K::UniformArrayDecl:
      checkTypeRef(llvm::cast<UniformArrayDeclExprAST>(node)->TypeName,
                   node->loc, "uniform");
      return;
    case K::StageVarDecl:
      checkTypeRef(llvm::cast<StageVarDeclAST>(node)->TypeName, node->loc,
                   "stage variable");
      return;
    case K::StorageBufferDecl:
      checkTypeRef(llvm::cast<StorageBufferDeclAST>(node)->ElemType, node->loc,
                   "buffer element");
      return;
    case K::StructDecl: {
      auto* sd = llvm::cast<StructDeclExprAST>(node);
      for (const auto& [ty, _name] : sd->Fields)
        checkTypeRef(ty, sd->loc, "struct field");
      return;
    }
    case K::Return: {
      auto* r = llvm::cast<ReturnStmtAST>(node);
      typeExpr(r->Expr);
      checkConvertible(r->Expr ? r->Expr->getType() : nullptr, currentRet_,
                       r->loc, "return");
      r->Expr = coerce(r->Expr, currentRet_);  // widen to the function ret type
      return;
    }

    default:
      // An expression in statement position (`f(x);`, `i++;`, …).
      typeExpr(node);
      return;
  }
}

// Type an expression subtree, returning (and stamping) its type. Conservative:
// nullptr where the rule isn't settled yet — never a diagnostic (except the
// literal range-check), so this can't reject a currently-valid shader.
const Type* SemanticAnalyzer::typeExpr(ExprAST* node) {
  if (!node) return nullptr;
  using K = ExprAST::Kind;
  const Type* t = nullptr;

  switch (node->getKind()) {
    case K::Number:
      typeNumberLiteral(llvm::cast<NumberExprAST>(node));
      return node->getType();
    case K::Boolean:
      node->setType(Ctx.getBoolTy());
      return node->getType();
    case K::Variable:
      t = lookupVar(llvm::cast<VariableExprAST>(node)->Name);
      break;
    case K::Unary: {
      auto* u = llvm::cast<UnaryExprAST>(node);
      const Type* o = typeExpr(u->Operand);
      if (u->Op == TokenKind::Not) {
        // GLSL: logical `!` applies only to bool. Reject `!int`/`!float` here so
        // a misuse is a located diagnostic rather than a codegen miscompile
        // (`!5` used to lower to `xor 5, 1` == 4). The result is always bool.
        if (o && !o->isBool()) {
          logErrorAt(u->loc, fmt::format(
              "logical '!' requires a bool operand, not '{}'", o->toString()));
          ++errorCount_;
        }
        t = Ctx.getBoolTy();
      } else {
        t = o;
      }
      break;
    }
    case K::Binary: {
      auto* b = llvm::cast<BinaryExprAST>(node);
      const Type* l = typeExpr(b->LHS);
      const Type* r = typeExpr(b->RHS);
      checkOperandTypes(b->Op, l, r, b->loc);  // reject bool arithmetic, etc.
      t = inferBinaryType(b->Op, l, r);
      // Promote both operands to their common type so they share one type before
      // codegen (e.g. `1.0 + 2` → `1.0 + (float)2`, `w * 2.0` → `w * vec3(2.0)`).
      // Operator context, so coerceOperand (scalar→vector broadcast allowed).
      // coercesOperands gates which ops promote (arithmetic, comparisons,
      // bitwise &|^; NOT shifts — their result type follows the LHS).
      if (coercesOperands(b->Op)) {
        if (const Type* common = commonOperandType(l, r)) {
          b->LHS = coerceOperand(b->LHS, common);
          b->RHS = coerceOperand(b->RHS, common);
        } else if (l && r && l->isVector() && r->isVector()) {
          // Two vectors with no common type ⇒ a size mismatch (`vec3 + vec2`).
          // Diagnose here, with location, instead of letting it fall through
          // untyped to a late codegen "Vector size mismatch". (Matrix operands
          // return nullptr too but aren't both-vectors, so they still defer.)
          logErrorAt(b->loc, fmt::format(
              "operator '{}' on vectors of different sizes ('{}' and '{}')",
              tokenKindName(b->Op), l->toString(), r->toString()));
          ++errorCount_;
        }
      }
      break;
    }
    case K::Ternary: {
      auto* tn = llvm::cast<TernaryExprAST>(node);
      typeExpr(tn->Cond);
      const Type* a = typeExpr(tn->ThenExpr);
      const Type* e = typeExpr(tn->ElseExpr);
      // GLSL §5.9: the two arms convert to a common type. Materialize that here
      // (widen the narrower arm) and stamp it, so codegen just builds the PHI —
      // the rank() reconcile in TernaryExprAST::codegen becomes the dead net.
      if (const Type* common = commonOperandType(a, e)) {
        // A scalar arm that doesn't widen to the common type is an illegal
        // implicit conversion (e.g. `cond ? true : 5` mixes bool and int).
        checkConvertible(a, common, tn->ThenExpr ? tn->ThenExpr->loc : tn->loc,
                         "ternary branch");
        checkConvertible(e, common, tn->ElseExpr ? tn->ElseExpr->loc : tn->loc,
                         "ternary branch");
        tn->ThenExpr = coerce(tn->ThenExpr, common);
        tn->ElseExpr = coerce(tn->ElseExpr, common);
        t = common;
      } else {
        t = (a == e) ? a : nullptr;  // structs / unsettled → defer
      }
      break;
    }
    case K::Call: {
      auto* c = llvm::cast<CallExprAST>(node);
      for (auto* arg : c->Args) typeExpr(arg);
      // A type-name callee is a constructor (`vec3(...)`, `Color(...)`) — its
      // arg coercion lives in the codegen constructor helpers (mixed shapes:
      // vec3(vec2,float), splats, …). A user function has known param types, so
      // we widen each argument to its parameter here. A builtin's result type
      // comes from its signature table (typeBuiltinCall); its args keep the
      // codegenBuiltin handling (overloaded shapes).
      if (const Type* ctor = resolveTypeName(c->Callee)) {
        t = ctor;
      } else if (auto it = funcSignatures_.find(c->Callee);
                 it != funcSignatures_.end()) {
        t = it->second.ret;
        const auto& params = it->second.params;
        for (size_t i = 0; i < c->Args.size() && i < params.size(); ++i) {
          checkConvertible(c->Args[i] ? c->Args[i]->getType() : nullptr,
                           params[i], c->Args[i]->loc, "argument");
          c->Args[i] = coerce(c->Args[i], params[i]);  // widen arg → param type
        }
      } else {
        std::vector<const Type*> argTys;
        argTys.reserve(c->Args.size());
        for (auto* a : c->Args) argTys.push_back(a ? a->getType() : nullptr);
        t = typeBuiltinCall(c->Callee, argTys);
      }
      break;
    }
    case K::MemberAccess: {
      auto* m = llvm::cast<MemberAccessExprAST>(node);
      t = typeMember(typeExpr(m->Object), m->Member);
      break;
    }
    case K::MemberAssignment: {
      auto* m = llvm::cast<MemberAssignmentExprAST>(node);
      const Type* o = typeExpr(m->Object);
      typeExpr(m->Init);
      t = typeMember(o, m->Member);  // the assigned field / swizzle type
      if (t) {  // widen the RHS to the member type (assignment context)
        checkConvertible(m->Init ? m->Init->getType() : nullptr, t, m->loc,
                         "assignment");
        m->Init = coerce(m->Init, t);
      }
      break;
    }
    case K::MatrixAccess: {
      auto* m = llvm::cast<MatrixAccessExprAST>(node);
      const Type* afterFirst = indexOnce(typeExpr(m->Object));
      typeExpr(m->Index);
      if (m->Index2) {
        typeExpr(m->Index2);
        t = indexOnce(afterFirst);  // mat[i][j] → scalar
      } else {
        t = afterFirst;  // arr[i] → elem,  mat[i] → column vector
      }
      break;
    }
    case K::MatrixAssignment: {
      auto* m = llvm::cast<MatrixAssignmentExprAST>(node);
      const Type* target = indexOnce(typeExpr(m->Object));  // a[i] → elem/column
      typeExpr(m->Index);
      if (m->Index2) {
        typeExpr(m->Index2);
        target = indexOnce(target);  // a[i][j] → scalar
      }
      typeExpr(m->RHS);
      if (target) {  // widen the RHS to the indexed element type
        checkConvertible(m->RHS ? m->RHS->getType() : nullptr, target, m->loc,
                         "assignment");
        m->RHS = coerce(m->RHS, target);
      }
      break;  // node result type unused
    }
    case K::ArrayInit: {
      auto* a = llvm::cast<ArrayInitExprAST>(node);
      const Type* elem = nullptr;
      for (auto* e : a->Elements) {
        const Type* et = typeExpr(e);
        if (!elem) elem = et;
      }
      if (elem && !a->Elements.empty())
        t = Ctx.getArrayTy(elem, static_cast<unsigned>(a->Elements.size()));
      break;
    }
    case K::PostfixIncr:
      t = typeExpr(llvm::cast<PostfixIncrExprAST>(node)->Target);
      break;

    default:
      return nullptr;  // Break/Continue/Discard/Prototype — not expressions
  }

  if (t) node->setType(t);
  return t;
}

// Result type of a binary operator from its operand types. Comparisons and
// logical ops are bool; arithmetic/bitwise follow the value rules. Returns
// nullptr for the cases that need the conversion step (matrix∘vector, etc.).
// A scalar that can be a vector element: int/uint/float. (No bvec/dvec — the
// Type vector ctor asserts this, so commonOperandType must never mint one.)
static bool isVectorElem(const glsl::Type* e) {
  return e->isInt() || e->isUint() || e->isFloat();
}

const Type* SemanticAnalyzer::commonOperandType(const Type* l, const Type* r) {
  if (!l || !r) return nullptr;
  if (l == r) return l;
  if (l->isScalar() && r->isScalar()) return higherRankScalar(l, r);

  // scalar ∘ vector (and the mirror): the scalar broadcasts; the element type is
  // the higher rank of the scalar and the vector's element.
  auto scalarVec = [&](const Type* s, const Type* v) -> const Type* {
    const Type* e = higherRankScalar(s, v->elementType());
    return isVectorElem(e) ? Ctx.getVectorTy(e, v->vectorSize()) : nullptr;
  };
  if (l->isScalar() && r->isVector()) return scalarVec(l, r);
  if (r->isScalar() && l->isVector()) return scalarVec(r, l);

  // vector ∘ vector, same size: promote to the higher-rank element type.
  if (l->isVector() && r->isVector() &&
      l->vectorSize() == r->vectorSize()) {
    const Type* e = higherRankScalar(l->elementType(), r->elementType());
    return isVectorElem(e) ? Ctx.getVectorTy(e, l->vectorSize()) : nullptr;
  }
  return nullptr;  // matrices, mismatched sizes — left to the codegen net
}

// Is a matrix binary op dimensionally legal? The SINGLE source of truth shared
// by inferMatrixBinary (result type) and checkOperandTypes (diagnostic), so the
// two never disagree. Assumes at least one operand is a matrix; returns false
// both for "not a matrix op this predicate covers" and "matrix op but wrong
// dimensions" — the caller distinguishes (inferMatrixBinary returns nullptr,
// checkOperandTypes emits a located error). Convention (confirmed): matCxR has C
// columns, R rows; matrixCols()=a_, matrixRows()=b_.
static bool matrixDimsOK(TokenKind op, const glsl::Type* l, const glsl::Type* r) {
  const bool mul = op == TokenKind::Multiply;
  if (mul && l->isMatrix() && r->isVector())
    return r->vectorSize() == l->matrixCols();   // matCxR * vecC
  if (mul && l->isVector() && r->isMatrix())
    return l->vectorSize() == r->matrixRows();   // vecR * matCxR
  if (mul && l->isMatrix() && r->isMatrix())
    return l->matrixCols() == r->matrixRows();   // matCxR * matKxC
  if (mul && (l->isMatrix() ^ r->isMatrix()) &&
      (l->isScalar() || r->isScalar()))
    return true;                                 // mat * scalar (scaling)
  if ((op == TokenKind::Plus || op == TokenKind::Minus) &&
      l->isMatrix() && r->isMatrix())
    return l->matrixCols() == r->matrixCols() &&
           l->matrixRows() == r->matrixRows();   // mat ± mat, same shape
  return false;
}

const Type* SemanticAnalyzer::inferMatrixBinary(TokenKind op, const Type* l,
                                                const Type* r) {
  if (!l || !r || (!l->isMatrix() && !r->isMatrix())) return nullptr;
  if (!matrixDimsOK(op, l, r)) return nullptr;  // illegal → checkOperandTypes errors
  const bool mul = op == TokenKind::Multiply;
  if (mul && l->isMatrix() && r->isVector())
    return Ctx.getVectorTy(Ctx.getFloatTy(), l->matrixRows());  // → vecR
  if (mul && l->isVector() && r->isMatrix())
    return Ctx.getVectorTy(Ctx.getFloatTy(), r->matrixCols());  // → vecC
  if (mul && l->isMatrix() && r->isMatrix())
    return Ctx.getMatrixTy(r->matrixCols(), l->matrixRows());   // → mat(colsR × rowsL)
  return l->isMatrix() ? l : r;  // mat*scalar, scalar*mat, mat±mat → the matrix
}

const Type* SemanticAnalyzer::inferBinaryType(TokenKind op, const Type* l,
                                              const Type* r) {
  if (isBoolResultOp(op)) return Ctx.getBoolTy();
  if (!l || !r) return nullptr;
  // §5.9: a shift's result type is the LEFT operand's type. The operands may be
  // of mixed signedness — the right operand only supplies a shift count — so we
  // do NOT promote to a common type here (and coercesOperands excludes shifts).
  if (isShift(op)) return l;
  // Matrices have their own rule and go BEFORE commonOperandType (which doesn't
  // know matrices). A dimension error returns nullptr here, but checkOperandTypes
  // emits the diagnostic — it is NOT a silent defer. The old `scalar ∘ !scalar →
  // composite` branch is gone: matrices land here, scalar∘vector in common.
  if (l->isMatrix() || r->isMatrix()) return inferMatrixBinary(op, l, r);
  if (const Type* c = commonOperandType(l, r)) return c;
  return nullptr;
}

const Type* SemanticAnalyzer::typeBuiltinCall(
    const std::string& name, const std::vector<const Type*>& args) {
  // Reductions: always a float scalar regardless of the (vector) arg shape.
  if (name == "dot" || name == "length" || name == "distance")
    return Ctx.getFloatTy();
  // Fixed-shape results.
  if (name == "cross") return Ctx.getVec3Ty();
  if (name == "texture" || name == "textureLod" || name == "imageLoad")
    return Ctx.getVec4Ty();
  // Statement-like builtins (no value).
  if (name == "imageStore" || name == "barrier" || name == "memoryBarrier" ||
      name == "groupMemoryBarrier")
    return Ctx.getVoidTy();
  // genType builtins: the result has the shape of the value argument — the first
  // vector arg if any (`max(float, vec3)` → vec3, `step(float, vec3)` → vec3),
  // else the first arg (all-scalar call).
  static const std::unordered_set<std::string> genType = {
      "sin",  "cos",   "tan",    "floor",      "ceil",   "round",
      "trunc","fract", "exp",    "log",        "exp2",   "log2",
      "sqrt", "abs",   "sign",   "normalize",  "mix",    "mod",
      "max",  "min",   "clamp",  "pow",        "reflect","fma",
      "step", "smoothstep", "dFdx", "dFdy",    "fwidth"};
  if (genType.count(name)) {
    for (const Type* a : args)
      if (a && a->isVector()) return a;
    return args.empty() ? nullptr : args[0];
  }
  return nullptr;  // unknown builtin → untyped (codegen net resolves it)
}

// Result type of `.member` on `objTy`: a vector swizzle (length 1 → element,
// 2..4 → vector) or a struct field (looked up through the type's decl pointer).
const Type* SemanticAnalyzer::typeMember(const Type* objTy,
                                         const std::string& member) {
  if (!objTy) return nullptr;
  if (objTy->isVector()) {
    size_t n = member.size();
    if (n == 1) return objTy->elementType();
    if (n >= 2 && n <= 4)
      return Ctx.getVectorTy(objTy->elementType(), static_cast<unsigned>(n));
    return nullptr;  // 5+ component swizzle — invalid, diagnosed in Step 3
  }
  if (objTy->isStruct() && !objTy->isIncompleteStruct()) {
    for (const auto& [fieldTy, fieldName] : objTy->structDecl()->Fields)
      if (fieldName == member) return resolveTypeName(fieldTy);
  }
  return nullptr;
}

// One level of `[]` indexing: array → element, matrix → column vector,
// vector → element scalar.
const Type* SemanticAnalyzer::indexOnce(const Type* t) {
  if (!t) return nullptr;
  if (t->isArray()) return t->arrayElementType();
  if (t->isMatrix()) return Ctx.getVectorTy(Ctx.getFloatTy(), t->matrixRows());
  if (t->isVector()) return t->elementType();
  return nullptr;
}

// ASSIGNMENT context (§4.1.10): wrap `e` in an ImplicitCastExprAST iff it
// converts implicitly to `target` — no scalar→vector broadcast, no bool. Used at
// init / re-assignment / return / argument / ternary arm.
//
// NOTE on matrices (an invariant ImplicitCastExprAST::codegen and Step a rely
// on): `target` CAN be a matrix here (`mat3 m = …`, a mat-returning fn, a mat
// parameter), but coerce NEVER wraps one — an identical matrix is the `et ==
// target` early-return, and convertsImplicitly() is false for any non-identical
// matrix pair (GLSL has no implicit matrix conversion: fixed shape, all float).
// commonOperandType() returns nullptr for matrices, so coerceOperand never sees
// one either. Net: no ImplicitCast is ever minted over a matrix type.
ExprAST* SemanticAnalyzer::coerce(ExprAST* e, const Type* target) {
  if (!e || !target) return e;
  const Type* et = e->getType();
  if (!et || et == target || !convertsImplicitly(et, target)) return e;
  auto* cast = Ctx.create<ImplicitCastExprAST>(e, target);
  cast->loc = e->loc;  // the conversion sits at the operand's position
  return cast;
}

// OPERATOR context (§5): coerce() plus a scalar broadcasting to a vector whose
// element it widens to (`w * 2.0` → `w * vec3(2.0)`). Binary operands only.
ExprAST* SemanticAnalyzer::coerceOperand(ExprAST* e, const Type* target) {
  if (!e || !target) return e;
  const Type* et = e->getType();
  if (!et || et == target) return e;
  bool ok = convertsImplicitly(et, target);
  if (!ok && target->isVector() && et->isScalar())
    ok = canWiden(et, target->elementType());  // §5 scalar broadcast
  if (!ok) return e;
  auto* cast = Ctx.create<ImplicitCastExprAST>(e, target);
  cast->loc = e->loc;
  return cast;
}

void SemanticAnalyzer::checkConvertible(const Type* from, const Type* to,
                                        SourceLocation loc, const char* role) {
  if (!from || !to || from == to) return;
  // Assignment context: anything that doesn't implicitly convert is an error —
  // narrowing (float→int), a scalar where a vector is required (float→vec3 needs
  // an explicit vec3(x)), a size/element/shape mismatch, bool↔numeric, etc.
  if (!convertsImplicitly(from, to)) {
    logErrorAt(loc, fmt::format("cannot implicitly convert '{}' to '{}' in {}",
                                from->toString(), to->toString(), role));
    ++errorCount_;
  }
}

// Binary-operator operand-domain check (the binary analogue of the unary `!`
// check). Best-effort: untyped operands (builtins) are skipped.
void SemanticAnalyzer::checkOperandTypes(TokenKind op, const Type* l,
                                         const Type* r, SourceLocation loc) {
  if (!l || !r) return;
  auto elem = [](const Type* t) { return t->isVector() ? t->elementType() : t; };
  auto numeric = [](const Type* e) {
    return e->isInt() || e->isUint() || e->isFloat() || e->isDouble();
  };
  auto integral = [](const Type* e) { return e->isInt() || e->isUint(); };

  // '%' is integer-only in GLSL (§5.9 — floats use mod()), so it joins the
  // bitwise/shift group, NOT arithmetic.
  const bool arith = op == TokenKind::Plus || op == TokenKind::Minus ||
                     op == TokenKind::Multiply || op == TokenKind::Divide;
  const bool relational = op == TokenKind::Less || op == TokenKind::LessEqual ||
                          op == TokenKind::Greater ||
                          op == TokenKind::GreaterEqual;
  const bool intOnly = op == TokenKind::Percent || isShift(op) ||
                       op == TokenKind::BitwiseAnd ||
                       op == TokenKind::BitwiseOr || op == TokenKind::BitwiseXor;

  // Matrix operands: a dimensionally-valid `*` / `+` / `-` (per matrixDimsOK,
  // the same predicate inferMatrixBinary uses) defers to that typing/lowering.
  // A dimension MISMATCH on `* + -` is diagnosed here (where loc exists) instead
  // of being left untyped — a silent dimension defer is exactly the false-accept
  // class we're closing. `/ % <` on a matrix falls through to the numeric/
  // integer check below, which rejects it ('mat3' isn't numeric/integer).
  if (l->isMatrix() || r->isMatrix()) {
    if (matrixDimsOK(op, l, r)) return;
    if (op == TokenKind::Multiply || op == TokenKind::Plus ||
        op == TokenKind::Minus) {
      logErrorAt(loc, fmt::format(
          "incompatible dimensions for '{}': '{}' and '{}'",
          tokenKindName(op), l->toString(), r->toString()));
      ++errorCount_;
      return;
    }
    // else: fall through to the numeric/integer rejection below.
  }

  if (arith || relational) {
    if (!numeric(elem(l)) || !numeric(elem(r))) {
      logErrorAt(loc, fmt::format(
          "operator '{}' requires numeric operands, got '{}' and '{}'",
          tokenKindName(op), l->toString(), r->toString()));
      ++errorCount_;
    }
  } else if (intOnly) {
    if (!integral(elem(l)) || !integral(elem(r))) {
      logErrorAt(loc, fmt::format(
          "operator '{}' requires integer operands, got '{}' and '{}'",
          tokenKindName(op), l->toString(), r->toString()));
      ++errorCount_;
    }
  }
}

// Type a numeric literal per GLSL §4.1: an integer literal is `int`, or `uint`
// with the `u` suffix; anything with a '.' or exponent is `float`. The literal
// value must fit the 32-bit width — `4294967296` does not, and that is a
// diagnostic here, not a silent wrap in codegen.
void SemanticAnalyzer::typeNumberLiteral(NumberExprAST* num) {
  if (!num->isInt) {
    num->setType(Ctx.getFloatTy());
    return;
  }
  // Width-aware bound: a `u` literal fills the unsigned range (2^32-1), a plain
  // one only the signed range (2^31-1). `int x = 3000000000` must NOT silently
  // wrap to a negative i32; it needs the `u` suffix.
  const double limit = num->isUnsigned ? 4294967295.0 : 2147483647.0;
  if (num->Val < 0.0 || num->Val > limit) {
    logErrorAt(num->loc,
               fmt::format("integer literal {} does not fit in {}",
                           static_cast<long long>(num->Val),
                           num->isUnsigned ? "uint" : "int"));
    ++errorCount_;
  }
  num->setType(num->isUnsigned ? Ctx.getUintTy() : Ctx.getIntTy());
}

void SemanticAnalyzer::validateTypeRefs(
    const std::vector<ExprAST*>& program) {
  for (auto* node : program) visit(node);
}

// ── Entry point ────────────────────────────────────────────────────────────
int SemanticAnalyzer::run(
    const std::vector<ExprAST*>& program) {
  collectStructNames(program);
  buildDependencies(program);
  detectCycles();

  enterScope();          // global scope: uniforms, stage vars, buffers
  bindGlobals(program);  // + function return types, before any body is typed
  validateTypeRefs(program);  // walk: validate type refs + stamp types
  leaveScope();
  return errorCount_;
}
