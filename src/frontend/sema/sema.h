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

class SemanticAnalyzer {
 public:
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

  // Visit helpers — recursive descent over a single subtree.
  void visit(const ExprAST* node);

  // Checks.
  bool isBuiltinType(const std::string& name) const;
  bool isKnownType(const std::string& name) const;
  void checkTypeRef(const std::string& name, int line, int col,
                    const char* role);

  // DFS state for detectCycles().
  bool hasCycleFrom(const std::string& start,
                    std::unordered_set<std::string>& visited,
                    std::unordered_set<std::string>& onStack);

  std::unordered_set<std::string> structNames_;
  std::unordered_map<std::string, std::vector<std::string>>
      structDependencies_;
  // Source position of each struct decl, for cycle diagnostics.
  std::unordered_map<std::string, std::pair<int, int>> structLocs_;

  int errorCount_ = 0;
};

#endif  // SEMA_H
