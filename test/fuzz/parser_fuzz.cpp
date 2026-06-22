// parser_fuzz.cpp — libFuzzer entry point for the parser + semantic pass.
//
// Goal: prove that no input string — including invalid, adversarial, or
// random garbage — can crash the parser or send it into an infinite loop.
// A parser that's user-facing must be hardened against this; a fuzzer is
// the cheapest way to find the cases hand-written tests miss.
//
// Build (see CMakeLists.txt `parser_fuzz` target): clang++ with
//   -fsanitize=fuzzer,address,undefined
//
// Run:
//   ./build/parser_fuzz                  # infinite fuzz (Ctrl-C to stop)
//   ./build/parser_fuzz -max_total_time=30
//   ./build/parser_fuzz corpus/          # seed with existing .src files
//
// On a crash, libFuzzer dumps the offending input to crash-<hash> and
// prints a sanitizer trace — reproduce with:
//   ./build/parser_fuzz crash-<hash>
//
// We do NOT run codegen here: that would touch LLVM globals and we'd
// have to InitializeModule per call (slow + state-leak prone). Parser +
// sema is where the input-driven complexity lives.

#include "../../src/frontend/ast/ast_context.h"
#include "../../src/frontend/parser/parser.h"
#include "../../src/frontend/sema/sema.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap input size — libFuzzer happily generates megabytes; the
    // realistic shader-source budget is a few KB and longer inputs just
    // slow the corpus without exploring new states.
    if (size > 64 * 1024) return 0;

    std::string source(reinterpret_cast<const char*>(data), size);
    ASTContext   astCtx;          // arena scoped to this single fuzz iteration
    auto nodes = ParseProgram(astCtx, source);

    // Only run sema on successfully-parsed programs — sema isn't designed
    // to walk a half-built AST. The parser owns input-validation; sema
    // owns whole-program type validation.
    if (!nodes.empty()) {
        SemanticAnalyzer sema(astCtx);
        (void)sema.run(nodes);
    }
    return 0;
}
