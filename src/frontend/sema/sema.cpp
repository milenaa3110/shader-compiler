// sema/sema.cpp — implementation of the post-parse semantic pass.

#include "sema.h"

#include <fmt/core.h>

#include "../error_utils_fmt.h"

// ────────────────────────────────────────────────────────────────────────────
// The set of types the lexer recognises as keywords — kept in sync with
// `tokToTypeName()` in the parser. Anything outside this set MUST be a
// user-declared struct.
// ────────────────────────────────────────────────────────────────────────────
bool SemanticAnalyzer::isBuiltinType(const std::string& name) const {
  static const std::unordered_set<std::string> kBuiltins = {
      "vec2",        "vec3",     "vec4",
      "double",      "float",    "int",      "uint",
      "bool",
      "mat2",        "mat3",     "mat4",
      "mat2x3",      "mat2x4",   "mat3x2",   "mat3x4",   "mat4x2",
      "mat4x3",
      "uvec2",       "uvec3",    "uvec4",
      "ivec2",       "ivec3",    "ivec4",
      "sampler2D",   "sampler3D",
      "samplerCube", "image2D",
      "void",
  };
  return kBuiltins.count(name) > 0;
}

bool SemanticAnalyzer::isKnownType(const std::string& name) const {
  return isBuiltinType(name) || structNames_.count(name) > 0;
}

void SemanticAnalyzer::checkTypeRef(const std::string& name, int line, int col,
                                    const char* role) {
  if (isKnownType(name)) return;
  logErrorAt(line, col,
             fmt::format("Unknown {} type '{}'", role, name));
  ++errorCount_;
}

// ── Pass 1 ─────────────────────────────────────────────────────────────────
void SemanticAnalyzer::collectStructNames(
    const std::vector<ExprAST*>& program) {
  for (auto* node : program) {
    if (auto* sd = llvm::dyn_cast_or_null<StructDeclExprAST>(node)) {
      if (!structNames_.insert(sd->Name).second) {
        logErrorAt(sd->line, sd->col,
                   fmt::format("Duplicate struct definition '{}'", sd->Name));
        ++errorCount_;
      }
      structLocs_[sd->Name] = {sd->line, sd->col};
    }
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
      int line = it != structLocs_.end() ? it->second.first  : 0;
      int col  = it != structLocs_.end() ? it->second.second : 0;
      logErrorAt(line, col,
                 fmt::format("Struct '{}' contains recursive definition",
                             name));
      ++errorCount_;
      // Erasing the dep prevents further DFS from re-reporting the same
      // cycle from a different start node.
      structDependencies_.erase(name);
    }
  }
}

// ── Pass 4 ─────────────────────────────────────────────────────────────────
void SemanticAnalyzer::visit(const ExprAST* node) {
  if (!node) return;
  using K = ExprAST::Kind;

  switch (node->getKind()) {
    case K::Function: {
      auto* fn = llvm::cast<FunctionAST>(node);
      // PrototypeAST isn't position-stamped by the parser; fall back to
      // the enclosing function's position so diagnostics land on the
      // `fn` keyword instead of line 0, col 0.
      const int line = fn->Proto && fn->Proto->line != 0 ? fn->Proto->line
                                                         : fn->line;
      const int col  = fn->Proto && fn->Proto->col  != 0 ? fn->Proto->col
                                                         : fn->col;
      if (fn->Proto) {
        const auto& ret = fn->Proto->RetType;
        if (!ret.empty() && !isKnownType(ret)) {
          checkTypeRef(ret, line, col, "return");
        }
        for (const auto& [ty, _name] : fn->Proto->Args) {
          checkTypeRef(ty, line, col, "parameter");
        }
      }
      visit(fn->Body);
      return;
    }
    case K::Block: {
      for (auto* s : llvm::cast<BlockExprAST>(node)->Statements)
        visit(s);
      return;
    }
    case K::If: {
      auto* i = llvm::cast<IfExprAST>(node);
      visit(i->Condition);
      visit(i->ThenExpr);
      visit(i->ElseExpr);
      return;
    }
    case K::While: {
      auto* w = llvm::cast<WhileExprAST>(node);
      visit(w->Condition);
      visit(w->Body);
      return;
    }
    case K::For: {
      auto* f = llvm::cast<ForExprAST>(node);
      visit(f->Init);
      visit(f->Condition);
      visit(f->Increment);
      visit(f->Body);
      return;
    }
    case K::Assignment: {
      auto* a = llvm::cast<AssignmentExprAST>(node);
      // VarType is empty for pure re-assignments (`a = 5;`); non-empty only
      // for declarations carried through this node (`vec3 a = ...;`).
      if (!a->VarType.empty()) {
        checkTypeRef(a->VarType, a->line, a->col, "variable");
      }
      visit(a->Init);
      return;
    }
    case K::ArrayDecl: {
      auto* a = llvm::cast<ArrayDeclExprAST>(node);
      checkTypeRef(a->ElementType, a->line, a->col, "array element");
      visit(a->Init);
      return;
    }
    case K::UniformDecl: {
      auto* u = llvm::cast<UniformDeclExprAST>(node);
      checkTypeRef(u->TypeName, u->line, u->col, "uniform");
      return;
    }
    case K::UniformArrayDecl: {
      auto* u = llvm::cast<UniformArrayDeclExprAST>(node);
      checkTypeRef(u->TypeName, u->line, u->col, "uniform");
      return;
    }
    case K::StageVarDecl: {
      auto* s = llvm::cast<StageVarDeclAST>(node);
      checkTypeRef(s->TypeName, s->line, s->col, "stage variable");
      return;
    }
    case K::StorageBufferDecl: {
      auto* b = llvm::cast<StorageBufferDeclAST>(node);
      checkTypeRef(b->ElemType, b->line, b->col, "buffer element");
      return;
    }
    case K::StructDecl: {
      auto* sd = llvm::cast<StructDeclExprAST>(node);
      for (const auto& [ty, _name] : sd->Fields) {
        checkTypeRef(ty, sd->line, sd->col, "struct field");
      }
      return;
    }
    case K::Return: {
      visit(llvm::cast<ReturnStmtAST>(node)->Expr);
      return;
    }
    default:
      // Expression-only nodes carry no type-name strings of their own.
      return;
  }
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
  validateTypeRefs(program);
  return errorCount_;
}
