// sema/sema.cpp — implementation of the post-parse semantic pass.

#include "sema.h"

#include <fmt/core.h>

#include "../../common/error_utils_fmt.h"
#include "../ast/ast_context.h"

using glsl::Type;

// Check if name is a basic builtin type.
bool SemanticAnalyzer::isBuiltinType(const std::string& name) const {
  static const std::unordered_set<std::string> kBuiltins = {
      "void",
#define BTYPE(Tok, Spelling) Spelling,
#include "../ast/builtin_types.def"
  };
  return kBuiltins.count(name) > 0;
}

// Check if name is a valid builtin, array, or user-defined struct type.
bool SemanticAnalyzer::isKnownType(const std::string& name) const {
  if (auto lb = name.find('[');
      lb != std::string::npos && !name.empty() && name.back() == ']')
    return isKnownType(name.substr(0, lb));
  return isBuiltinType(name) || structNames_.count(name) > 0;
}

// Validate type existence and log diagnostic on failure.
void SemanticAnalyzer::checkTypeRef(const std::string& name, SourceLocation loc,
                                    const char* role) {
  if (isKnownType(name)) return;
  logErrorAt(loc, fmt::format("Unknown {} type '{}'", role, name));
  ++errorCount_;
}

// Map a type name string to its canonical glsl::Type descriptor.
const Type* SemanticAnalyzer::resolveTypeName(llvm::StringRef n) {
  if (n == "void") return Ctx.getVoidTy();

  if (auto lb = n.find('['); lb != llvm::StringRef::npos && n.ends_with("]")) {
    llvm::StringRef numStr = n.substr(lb + 1, n.size() - lb - 2);
    int cnt = 0;
    if (!numStr.getAsInteger(10, cnt) && cnt > 0)
      if (const Type* elem = resolveTypeName(n.substr(0, lb)))
        return Ctx.getArrayTy(elem, static_cast<unsigned>(cnt));
    return nullptr;
  }
  using SK = Type::SamplerKind;
#define BTYPE_SCALAR(Tok, Spelling, GlslKind, LlvmGetter) \
  if (n == Spelling) return Ctx.get##GlslKind##Ty();
#define BTYPE_VECTOR(Tok, Spelling, GlslElem, LlvmElem, N) \
  if (n == Spelling) return Ctx.getVectorTy(Ctx.get##GlslElem##Ty(), N);
#define BTYPE_MATRIX(Tok, Spelling, Cols, Rows) \
  if (n == Spelling) return Ctx.getMatrixTy(Cols, Rows);
#define BTYPE_SAMPLER(Tok, Spelling, Kind, Dim, Arrayed, IsImage) \
  if (n == Spelling) return Ctx.getSamplerTy(SK::Kind);
#include "../ast/builtin_types.def"
  if (structNames_.count(std::string(n))) return Ctx.getStructTy(n);
  return nullptr;
}

// Scope helpers

// Insert a variable declaration into the current lexical scope.
void SemanticAnalyzer::bindLocal(const std::string& name, const Type* t,
                                 bool isConst) {
  if (t && !scopes_.empty()) scopes_.back()[name] = VarInfo{t, isConst};
}
// Look up a variable by walking the scope stack from inner to outer.
const Type* SemanticAnalyzer::lookupVar(const std::string& name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto f = it->find(name);
    if (f != it->end()) return f->second.type;
  }
  return nullptr;
}
// Check if a variable is declared constant in the visible scope.
bool SemanticAnalyzer::isConstVar(const std::string& name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto f = it->find(name);
    if (f != it->end()) return f->second.isConst;
  }
  return false;
}

// Traverse an lvalue reference chain to enforce immutability on constant data roots.
bool SemanticAnalyzer::reportIfConstWrite(const ExprAST* target,
                                          SourceLocation loc) {
  using K = ExprAST::Kind;
  while (target) {
    switch (target->getKind()) {
      case K::Variable: {
        const auto* v = llvm::cast<VariableExprAST>(target);
        if (isConstVar(v->Name)) {
          logErrorAt(loc, fmt::format(
                              "cannot assign to const variable '{}'", v->Name));
          ++errorCount_;
          return true;
        }
        return false;
      }
      case K::MemberAccess:
        target = llvm::cast<MemberAccessExprAST>(target)->Object;
        break;
      case K::MatrixAccess:
        target = llvm::cast<MatrixAccessExprAST>(target)->Object;
        break;
      default:
        return false;  // not rooted in a plain variable — nothing to enforce
    }
  }
  return false;
}

// Bind global declarations and function signatures to support out-of-order resolution.
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

// Pass 1
// Collect struct names and complete canonical types using the incomplete-type protocol.
void SemanticAnalyzer::collectStructNames(
    const std::vector<ExprAST*>& program) {
  for (auto* node : program) {
    auto* sd = llvm::dyn_cast_or_null<StructDeclExprAST>(node);
    if (!sd) continue;

    const Type* st = Ctx.getStructTy(sd->Name);
    if (!st->isIncompleteStruct()) {
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
    st->setStructDecl(sd);
  }
}

// Pass 2
// Construct the direct struct composition dependency graph.
void SemanticAnalyzer::buildDependencies(
    const std::vector<ExprAST*>& program) {
  for (auto* node : program) {
    auto* sd = llvm::dyn_cast_or_null<StructDeclExprAST>(node);
    if (!sd) continue;
    std::vector<std::string> deps;
    for (const auto& [fieldType, fieldName] : sd->Fields) {
      if (structNames_.count(fieldType) > 0) deps.push_back(fieldType);
    }
    if (!deps.empty()) structDependencies_[sd->Name] = std::move(deps);
  }
}

// Pass 3
// Recursively detect cyclic dependancy in the struct dependency graph using DFS.
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
      structDependencies_.erase(name);
    }
  }
}

// Rank for the implicit-conversion order
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

// implicit conversions are widening only
static bool canWiden(const glsl::Type* from, const glsl::Type* to) {
  auto numeric = [](const glsl::Type* t) {
    return t->isInt() || t->isUint() || t->isFloat() || t->isDouble();
  };
  return numeric(from) && numeric(to) && scalarRank(from) <= scalarRank(to);
}

// Implicit conversion in assignment context.
// Handles scalar-to-scalar and component-wise vector widening to the same size.
static bool convertsImplicitly(const glsl::Type* from, const glsl::Type* to) {
  if (!from || !to) return false;
  if (from == to) return true;
  if (from->isScalar() && to->isScalar()) return canWiden(from, to);
  if (from->isVector() && to->isVector() &&
      from->vectorSize() == to->vectorSize())
    return canWiden(from->elementType(), to->elementType());
  return false;
}

// Returns true for operators where operands undergo arithmetic promotion to a common type.
// Logical and shift operations are excluded.
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
// Scope management, symbol table binding, and declaration type validation.
// Actual type mismatch verification is deferred to the conversion check phase.
void SemanticAnalyzer::visit(ExprAST* node) {
  if (!node) return;
  using K = ExprAST::Kind;

  switch (node->getKind()) {
    case K::Function: {
      auto* fn = llvm::cast<FunctionAST>(node);
      // Fallback to function keyword location if prototype is unmapped.
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
      // Bind stage-specific implicit variables for entry points.
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
      enterScope();
      visit(f->Init);
      typeExpr(f->Condition);
      visit(f->Increment);
      visit(f->Body);
      leaveScope();
      return;
    }
    case K::Assignment: {
      auto* a = llvm::cast<AssignmentExprAST>(node);
      typeExpr(a->Init);
      if (!a->VarType.empty()) { // Variable declaration with initializer
        checkTypeRef(a->VarType, a->loc, "variable");
        const Type* target = resolveTypeName(a->VarType);
        bindLocal(a->VarName, target, a->IsConst);
        checkConvertible(a->Init ? a->Init->getType() : nullptr, target, a->loc,
                         "initialization");
        a->Init = coerce(a->Init, target);  // widen initializer to decl type
      } else if (const Type* target = lookupVar(a->VarName)) {
        if (isConstVar(a->VarName)) { // Pure re-assignment
          logErrorAt(a->loc, fmt::format(
                                 "cannot assign to const variable '{}'",
                                 a->VarName));
          ++errorCount_;
        }
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
        bindLocal(a->Name, Ctx.getArrayTy(e, static_cast<unsigned>(a->Size)),
                  a->IsConst);
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
      typeExpr(node);
      return;
  }
}

// Deduce, set, and return the type of an expression AST subtree.
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
      // Promote operands to their common type.
      if (coercesOperands(b->Op)) {
        if (const Type* common = commonOperandType(l, r)) {
          b->LHS = coerceOperand(b->LHS, common);
          b->RHS = coerceOperand(b->RHS, common);
        } else if (l && r && l->isVector() && r->isVector()) {
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
      // Harmonize ternary arm types.
      if (const Type* common = commonOperandType(a, e)) {
        checkConvertible(a, common, tn->ThenExpr ? tn->ThenExpr->loc : tn->loc,
                         "ternary branch");
        checkConvertible(e, common, tn->ElseExpr ? tn->ElseExpr->loc : tn->loc,
                         "ternary branch");
        tn->ThenExpr = coerce(tn->ThenExpr, common);
        tn->ElseExpr = coerce(tn->ElseExpr, common);
        t = common;
      } else {
        t = (a == e) ? a : nullptr;
      }
      break;
    }
    case K::Call: {
      auto* c = llvm::cast<CallExprAST>(node);
      for (auto* arg : c->Args) typeExpr(arg);
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
      reportIfConstWrite(m->Object, m->loc);
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
      checkIndexType(m->Index);
      if (m->Index2) {
        typeExpr(m->Index2);
        checkIndexType(m->Index2);
        t = indexOnce(afterFirst);  // mat[i][j] → scalar
      } else {
        t = afterFirst;  // arr[i] → elem,  mat[i] → column vector
      }
      break;
    }
    case K::MatrixAssignment: {
      auto* m = llvm::cast<MatrixAssignmentExprAST>(node);
      reportIfConstWrite(m->Object, m->loc);
      const Type* target = indexOnce(typeExpr(m->Object));  // a[i] → elem/column
      typeExpr(m->Index);
      checkIndexType(m->Index);
      if (m->Index2) {
        typeExpr(m->Index2);
        checkIndexType(m->Index2);
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
    case K::IncrDecr: {
      auto* p = llvm::cast<IncrDecrExprAST>(node);
      reportIfConstWrite(p->Target, node->loc);
      t = typeExpr(p->Target);
      break;
    }

    default:
      return nullptr;  // Break/Continue/Discard/Prototype — not expressions
  }

  if (t) node->setType(t);
  return t;
}

// True if the type can act as a component inside a GLSL vector.  
static bool isVectorElem(const glsl::Type* e) {
  return e->isInt() || e->isUint() || e->isFloat();
}

// Evaluates the promoted type of two operands using GLSL value rules.
const Type* SemanticAnalyzer::commonOperandType(const Type* l, const Type* r) {
  if (!l || !r) return nullptr;
  if (l == r) return l;
  if (l->isScalar() && r->isScalar()) return higherRankScalar(l, r);

  // Scalar-vector mixing
  auto scalarVec = [&](const Type* s, const Type* v) -> const Type* {
    const Type* e = higherRankScalar(s, v->elementType());
    return isVectorElem(e) ? Ctx.getVectorTy(e, v->vectorSize()) : nullptr;
  };
  if (l->isScalar() && r->isVector()) return scalarVec(l, r);
  if (r->isScalar() && l->isVector()) return scalarVec(r, l);

  // Vector-vector matching
  if (l->isVector() && r->isVector() &&
      l->vectorSize() == r->vectorSize()) {
    const Type* e = higherRankScalar(l->elementType(), r->elementType());
    return isVectorElem(e) ? Ctx.getVectorTy(e, l->vectorSize()) : nullptr;
  }
  return nullptr;
}

// Verifies if a matrix binary operation matches standard layout dimensions (matCxR has C columns, R rows).
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

// Infer result type of matrix-based binary operations.
const Type* SemanticAnalyzer::inferMatrixBinary(TokenKind op, const Type* l,
                                                const Type* r) {
  if (!l || !r || (!l->isMatrix() && !r->isMatrix())) return nullptr;
  if (!matrixDimsOK(op, l, r)) return nullptr;  // illegal -> checkOperandTypes errors
  const bool mul = op == TokenKind::Multiply;
  if (mul && l->isMatrix() && r->isVector())
    return Ctx.getVectorTy(Ctx.getFloatTy(), l->matrixRows());  // -> vecR
  if (mul && l->isVector() && r->isMatrix())
    return Ctx.getVectorTy(Ctx.getFloatTy(), r->matrixCols());  // -> vecC
  if (mul && l->isMatrix() && r->isMatrix())
    return Ctx.getMatrixTy(r->matrixCols(), l->matrixRows());   // -> mat(colsR × rowsL)
  return l->isMatrix() ? l : r;  // mat*scalar, scalar*mat, mat±mat → the matrix
}

// Infer the expected output type of a generic binary operator.
const Type* SemanticAnalyzer::inferBinaryType(TokenKind op, const Type* l,
                                              const Type* r) {
  if (isBoolResultOp(op)) return Ctx.getBoolTy();
  if (!l || !r) return nullptr;
  if (isShift(op)) return l;
  if (l->isMatrix() || r->isMatrix()) return inferMatrixBinary(op, l, r);
  if (const Type* c = commonOperandType(l, r)) return c;
  return nullptr;
}

// Look up and determine the return type of a standard GLSL builtin function.
const Type* SemanticAnalyzer::typeBuiltinCall(
    const std::string& name, const std::vector<const Type*>& args) {
  if (name == "dot" || name == "length" || name == "distance")
    return Ctx.getFloatTy();
  if (name == "cross") return Ctx.getVec3Ty();
  if (name == "texture" || name == "textureLod" || name == "imageLoad")
    return Ctx.getVec4Ty();
  if (name == "imageStore" || name == "barrier" || name == "memoryBarrier" ||
      name == "groupMemoryBarrier")
    return Ctx.getVoidTy();

    // genType builtins mirror the vector shape of their composite argument.
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
  return nullptr;
}

// Resolve the return type of a field access (.member) on vector or struct types.
const Type* SemanticAnalyzer::typeMember(const Type* objTy,
                                         const std::string& member) {
  if (!objTy) return nullptr;
  if (objTy->isVector()) {
    size_t n = member.size();
    if (n == 1) return objTy->elementType();
    if (n >= 2 && n <= 4)
      return Ctx.getVectorTy(objTy->elementType(), static_cast<unsigned>(n));
    return nullptr;
  }
  if (objTy->isStruct() && !objTy->isIncompleteStruct()) {
    for (const auto& [fieldTy, fieldName] : objTy->structDecl()->Fields)
      if (fieldName == member) return resolveTypeName(fieldTy);
  }
  return nullptr;
}

// Evaluate type after a single layer of subscript indexing.
const Type* SemanticAnalyzer::indexOnce(const Type* t) {
  if (!t) return nullptr;
  if (t->isArray()) return t->arrayElementType();
  if (t->isMatrix()) return Ctx.getVectorTy(Ctx.getFloatTy(), t->matrixRows());
  if (t->isVector()) return t->elementType();
  return nullptr;
}

// Injects an ImplicitCastExprAST if 'e' implicitly converts to 'target' under assignment rules.
ExprAST* SemanticAnalyzer::coerce(ExprAST* e, const Type* target) {
  if (!e || !target) return e;
  const Type* et = e->getType();
  if (!et || et == target || !convertsImplicitly(et, target)) return e;
  auto* cast = Ctx.create<ImplicitCastExprAST>(e, target);
  cast->loc = e->loc;
  return cast;
}

// Injects an ImplicitCastExprAST under operator rules, allowing scalar-to-vector broadcast.
ExprAST* SemanticAnalyzer::coerceOperand(ExprAST* e, const Type* target) {
  if (!e || !target) return e;
  const Type* et = e->getType();
  if (!et || et == target) return e;
  bool ok = convertsImplicitly(et, target);
  if (!ok && target->isVector() && et->isScalar())
    ok = canWiden(et, target->elementType());
  if (!ok) return e;
  auto* cast = Ctx.create<ImplicitCastExprAST>(e, target);
  cast->loc = e->loc;
  return cast;
}

// Emits an error diagnostic if type conversion is impossible in assignment contexts.
void SemanticAnalyzer::checkConvertible(const Type* from, const Type* to,
                                        SourceLocation loc, const char* role) {
  if (!from || !to || from == to) return;
  if (!convertsImplicitly(from, to)) {
    logErrorAt(loc, fmt::format("cannot implicitly convert '{}' to '{}' in {}",
                                from->toString(), to->toString(), role));
    ++errorCount_;
  }
}

// Validates that an array/matrix subscript index evaluates to an integral type.
void SemanticAnalyzer::checkIndexType(ExprAST* idx) {
  if (!idx) return;
  const Type* t = idx->getType();
  if (t && !t->isIntegral()) {
    logErrorAt(idx->loc,
               fmt::format("array/matrix index must be an integer, not '{}'",
                           t->toString()));
    ++errorCount_;
  }
}

// Enforces structural and domain constraints on binary expression operands.
void SemanticAnalyzer::checkOperandTypes(TokenKind op, const Type* l,
                                         const Type* r, SourceLocation loc) {
  if (!l || !r) return;
  auto elem = [](const Type* t) { return t->isVector() ? t->elementType() : t; };
  auto numeric = [](const Type* e) {
    return e->isInt() || e->isUint() || e->isFloat() || e->isDouble();
  };
  auto integral = [](const Type* e) { return e->isInt() || e->isUint(); };

  const bool arith = op == TokenKind::Plus || op == TokenKind::Minus ||
                     op == TokenKind::Multiply || op == TokenKind::Divide;
  const bool relational = op == TokenKind::Less || op == TokenKind::LessEqual ||
                          op == TokenKind::Greater ||
                          op == TokenKind::GreaterEqual;
  const bool intOnly = op == TokenKind::Percent || isShift(op) ||
                       op == TokenKind::BitwiseAnd ||
                       op == TokenKind::BitwiseOr || op == TokenKind::BitwiseXor;

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

// Types numeric literal and validates it fits in 32-bit width constraints.
void SemanticAnalyzer::typeNumberLiteral(NumberExprAST* num) {
  if (!num->isInt) {
    num->setType(Ctx.getFloatTy());
    return;
  }
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

// Main execution entry point for the post-parse semantic validation pass.
int SemanticAnalyzer::run(
    const std::vector<ExprAST*>& program) {
  collectStructNames(program);
  buildDependencies(program);
  detectCycles();

  enterScope();
  bindGlobals(program);
  validateTypeRefs(program);
  leaveScope();
  checkStageVarLocations(program);
  checkStageVarTypes(program);
  return errorCount_;
}

// Pass 5: 
// Detect location collisions for stage in/out variables.
void SemanticAnalyzer::checkStageVarLocations(
    const std::vector<ExprAST*>& program) {
  auto checkDir = [&](bool wantInput) {
    std::unordered_map<int, std::string> used;
    int nextAuto = 0;
    for (auto* node : program) {
      auto* s = llvm::dyn_cast_or_null<StageVarDeclAST>(node);
      if (!s || s->isInput != wantInput) continue;
      int loc = s->location >= 0 ? s->location : nextAuto++;
      auto [it, inserted] = used.emplace(loc, s->Name);
      if (!inserted) {
        logErrorAt(s->loc,
                   fmt::format("{} location {} of '{}' collides with '{}'",
                               wantInput ? "input" : "output", loc, s->Name,
                               it->second));
        ++errorCount_;
      }
    }
  };
  checkDir(true);
  checkDir(false);
}

// True if `t` is a double, or an array/struct that transitively contains one.
bool SemanticAnalyzer::typeContainsDouble(const glsl::Type* t) {
  if (!t) return false;
  if (t->isDouble()) return true;
  if (t->isArray()) return typeContainsDouble(t->arrayElementType());
  if (t->isStruct() && t->structDecl()) {
    // Field type names resolve through the same canonicalization as decls; an
    // incomplete struct (decl == nullptr) contributes nothing here.
    for (const auto& [fieldTy, _name] : t->structDecl()->Fields)
      if (typeContainsDouble(resolveTypeName(fieldTy))) return true;
  }
  return false;
}

// Pass 6:
// Reject double on interpolated stage I/O. The module's stage comes from its
// entry point; `in`/`out` then map to attribute/varying/fragment-output roles.
void SemanticAnalyzer::checkStageVarTypes(
    const std::vector<ExprAST*>& program) { 
  std::optional<ShaderStage> stage;
  for (auto* node : program)
    if (auto* fn = llvm::dyn_cast_or_null<FunctionAST>(node))
      if (fn->Attrs.isEntry && fn->Attrs.stage) { stage = fn->Attrs.stage; break; }
  if (!stage) return;  // library/compute module — no interpolated varyings.

  for (auto* node : program) {
    auto* s = llvm::dyn_cast_or_null<StageVarDeclAST>(node);
    if (!s) continue;
    // Interpolated surfaces: a vertex `out` and a fragment `in` are the two
    // ends of the same perspective-interpolated varying.
    const bool interpolated =
        (*stage == ShaderStage::Vertex && !s->isInput) ||
        (*stage == ShaderStage::Fragment && s->isInput);
    if (!interpolated) continue;
    if (typeContainsDouble(resolveTypeName(s->TypeName))) {
      logErrorAt(s->loc,
                 fmt::format("'double' is not allowed on interpolated stage {} "
                             "'{}' (declare it on a vertex input / fragment "
                             "output, or use a 32-bit type)",
                             s->isInput ? "input" : "output", s->Name));
      ++errorCount_;
    }
  }
}
