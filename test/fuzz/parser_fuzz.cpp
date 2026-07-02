#include "../../src/frontend/ast/ast_context.h"
#include "../../src/frontend/parser/parser.h"
#include "../../src/frontend/sema/sema.h"

#include <cstddef>
#include <cstdint>
#include <string>

/// libFuzzer entry point for hardening the compiler frontend against
/// malformed, adversarial, or random inputs.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Cap input size at 64KB to optimize execution speed and prevent the fuzzing
  // engine from generating redundantly large source mutations.
  if (size > 64 * 1024)
    return 0;

  std::string source(reinterpret_cast<const char*>(data), size);
  
  // Local arena allocator scoped strictly to the current fuzzing iteration.
  ASTContext astCtx;
  auto nodes = ParseProgram(astCtx, source);

  // Invoke type-checking and invariant validation only if the syntax tree 
  // was successfully built. Sema expects a structurally complete AST.
  if (!nodes.empty()) {
    SemanticAnalyzer sema(astCtx);
    (void)sema.run(nodes);
  }

  return 0;
}